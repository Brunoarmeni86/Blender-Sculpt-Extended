/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included inside multires_layers: transactional, topology-preserving detail transfer. */
bool symmetrize_active_detail(Object &object,
                             const float3 &origin,
                             const float3 &normal,
                             const float source_sign,
                             const float tolerance,
                             const bool respect_mask,
                             std::string &error)
{
  const auto finite = [](const float3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
  Mesh *mesh = BKE_mesh_from_object(&object);
  auto *mmd = reinterpret_cast<MultiresModifierData *>(BKE_modifiers_findby_type(&object, eModifierType_Multires));
  MultiresSculptLayer *layer = mmd ? active(*mmd) : nullptr;
  auto fail = [&](const char *message) { error = message; return false; };
  if (!mesh || mesh->key || mesh->id.us > 1 || mesh->id.lib || object.id.lib || !layer || !layer->disps ||
      layer->level < 1 || layer->level > 13 || layer->level != mmd->totlvl ||
      layer->grids_num != mesh->corners_num ||
      !(layer->flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
      !std::isfinite(layer->strength) || std::abs(layer->strength) < 1e-8f)
  {
    return fail("Select a visible, non-zero-strength Multires detail layer; no shared mesh or shape keys");
  }
  if (!std::isfinite(tolerance) || tolerance <= 0 ||
      !finite(origin) || !finite(normal) ||
      std::abs(math::length(normal) - 1.0f) > 1e-4f)
  {
    return fail("Invalid symmetry plane or correspondence distance");
  }
  const int side = subdiv::grid_size_from_level(layer->level), area = side * side;
  /* Bound the transaction before allocating dense payloads. No per-stroke cache. */
  if (int64_t(mesh->corners_num) * area * sizeof(float3) > 512LL * 1024 * 1024) {
    return fail("Preview transaction exceeds 512 MiB; use a lower stored subdivision level");
  }
  for (const MDisps &grid : Span(layer->disps, layer->grids_num)) {
    if (grid.disps && (grid.level != layer->level || grid.totdisp != area)) {
      return fail("Incompatible layer grid metadata");
    }
  }
  const auto reflect = [&](const float3 &p) {
    return p - 2.0f * normal * math::dot(p - origin, normal);
  };
  const auto reflect_vector = [&](const float3 &v) {
    return v - 2.0f * normal * math::dot(v, normal);
  };
  const Span<float3> positions = mesh->vert_positions();
  for (const float3 &p : positions) { if (!finite(p)) return fail("Non-finite cage coordinate"); }
  KDTree<float3> *tree = kdtree_new<float3>(positions.size());
  for (const int i : positions.index_range()) kdtree_insert(tree, i, positions[i]);
  kdtree_balance(tree);
  Array<int> vertex_map(positions.size(), -1);
  bool mapping_ok = true;
  for (const int i : positions.index_range()) {
    KDTreeNearest<float3> nearest[2];
    const int count = kdtree_find_nearest_n(tree, reflect(positions[i]), nearest, 2);
    if (!count || nearest[0].dist > tolerance ||
        (count > 1 && nearest[1].dist - nearest[0].dist < 1e-7f)) {
      mapping_ok = false;
      break;
    }
    vertex_map[i] = nearest[0].index;
  }
  kdtree_free(tree);
  if (mapping_ok) {
    for (const int i : positions.index_range()) {
      if (vertex_map[vertex_map[i]] != i) mapping_ok = false;
    }
  }
  if (!mapping_ok) return fail("Cage vertices have no unique reciprocal mirror correspondence");
  const OffsetIndices faces = mesh->faces();
  const Span<int> verts = mesh->corner_verts();
  std::map<std::vector<int>, int> lookup;
  for (const int f : faces.index_range()) {
    std::vector<int> key;
    for (const int c : faces[f]) key.push_back(verts[c]);
    std::sort(key.begin(), key.end());
    if (!lookup.emplace(key, f).second) return fail("Duplicate cage faces are ambiguous");
  }
  Array<int> corner_map(mesh->corners_num, -1);
  for (const int f : faces.index_range()) {
    const IndexRange face = faces[f];
    std::vector<int> key;
    for (const int c : face) key.push_back(vertex_map[verts[c]]);
    std::sort(key.begin(), key.end());
    auto found = lookup.find(key);
    if (found == lookup.end()) return fail("Cage topology is not mirror compatible");
    const IndexRange target = faces[found->second];
    for (const int j : IndexRange(face.size())) {
      const int c = face[j], next = face[(j + 1) % face.size()];
      for (const int k : IndexRange(target.size())) {
        if (verts[target[k]] == vertex_map[verts[c]] &&
            verts[target[(k + target.size() - 1) % target.size()]] == vertex_map[verts[next]]) {
          corner_map[c] = target[k];
          break;
        }
      }
      if (corner_map[c] < 0) return fail("Mirrored face winding does not match");
    }
  }
  for (const int c : corner_map.index_range()) {
    if (corner_map[corner_map[c]] != c) return fail("Non-reciprocal corner mapping");
  }
  const GridPaintMask *masks = respect_mask ? static_cast<const GridPaintMask *>(
      CustomData_get_layer(&mesh->corner_data, CD_GRID_PAINT_MASK)) : nullptr;
  if (respect_mask && !masks) {
    const VArray<float> point_masks = mesh->attributes().lookup_or_default<float>(
        ".sculpt_mask", AttrDomain::Point, 0.0f).varray;
    for (const int i : positions.index_range()) {
      if (point_masks[i] > 0) return fail("Commit the current point mask before detail transfer");
    }
  }
  auto mask_at = [&](int grid, float u, float v) {
    if (!masks || !masks[grid].data) return 0.0f;
    const GridPaintMask &mask = masks[grid];
    if (mask.level < 1 || mask.level > 13) return 1.0f;
    const int n = subdiv::grid_size_from_level(mask.level);
    const float x = u * (n - 1), y = v * (n - 1);
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, n - 1), y1 = std::min(y0 + 1, n - 1);
    const float a = math::interpolate(mask.data[y0*n+x0], mask.data[y0*n+x1], x-x0);
    const float b = math::interpolate(mask.data[y1*n+x0], mask.data[y1*n+x1], x-x0);
    const float value = math::interpolate(a, b, y-y0);
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
  };
  /* Only the coarse CCG is materialized; limit derivatives remain queryable at any UV. */
  Mesh *cage = BKE_mesh_copy_for_eval(*mesh);
  struct CageOwner { Mesh *value; ~CageOwner() { BKE_id_free(nullptr, value); } } cage_owner{cage};
  if (!base_deltas_apply(*mmd, cage->vert_positions_for_write())) return fail("Incompatible L0 layer offsets");
  cage->tag_positions_changed();
  subdiv::Settings settings;
  BKE_multires_subdiv_settings_init(&settings, mmd);
  subdiv::Subdiv *subdiv = subdiv::new_from_mesh(&settings, cage);
  if (!subdiv) return fail("Cannot create limit-surface evaluator");
  if (!subdiv::eval_begin_from_mesh(subdiv, cage, subdiv::SUBDIV_EVALUATOR_TYPE_CPU)) {
    subdiv::free(subdiv);
    return fail("Cannot initialize limit-surface evaluator");
  }
  SubdivToCCGSettings ccg_settings{};
  ccg_settings.resolution = 3;
  std::unique_ptr<SubdivCCG> ccg = BKE_subdiv_to_ccg(*subdiv, ccg_settings, *cage, nullptr);
  if (!ccg) return fail("Cannot create coarse surface evaluator");
  MultiresReshapeContext context{};
  if (!multires_reshape_context_create_from_ccg_evaluation(&context, ccg.get(), cage, layer->level)) {
    multires_reshape_context_free(&context);
    return fail("Cannot initialize grid correspondence evaluator");
  }
  struct ContextOwner { MultiresReshapeContext *value; ~ContextOwner() { multires_reshape_context_free(value); } } context_owner{&context};
  struct Payload {
    float (*data)[3] = nullptr;
    ~Payload() { if (data) MEM_delete(data); }
  };
  std::unique_ptr<Payload[]> staged(new Payload[layer->grids_num]);
  int changed = 0;
  const float inv = 1.0f / (side - 1);
  for (const int g : IndexRange(layer->grids_num)) {
    const int source_grid = corner_map[g];
    const MDisps &source = layer->disps[source_grid], &destination = layer->disps[g];
    if (!source.disps && !destination.disps) continue;
    for (int y = 0; y < side; y++) {
      for (int x = 0; x < side; x++) {
        const float u = x * inv, v = y * inv;
        float3 p; float3x3 target_basis;
        const GridCoord target_coord{g, u, v};
        multires_reshape_evaluate_base_mesh_limit_at_grid(&context, &target_coord, p, target_basis);
        if (math::dot(p - origin, normal) * source_sign >= -1e-7f) continue;
        const float weight = 1.0f - mask_at(g, u, v);
        if (weight == 0.0f) continue;
        if (std::abs(math::determinant(target_basis)) < 1e-10f) return fail("Degenerate destination surface frame");
        float3 desired(0.0f);
        if (source.disps) {
          float3 sp; float3x3 source_basis;
          const GridCoord source_coord{source_grid, v, u};
          multires_reshape_evaluate_base_mesh_limit_at_grid(&context, &source_coord, sp, source_basis);
          if (std::abs(math::determinant(source_basis)) < 1e-10f) return fail("Degenerate source surface frame");
          desired = math::invert(target_basis) * reflect_vector(source_basis * float3(source.disps[x * side + y]));
        }
        const int i = y * side + x;
        const float3 old = destination.disps ? float3(destination.disps[i]) : float3(0.0f);
        const float3 value = math::interpolate(old, desired, weight);
        if (!finite(value)) return fail("Non-finite layer displacement; nothing transferred");
        if (value == old) continue;
        if (!staged[g].data) {
          staged[g].data = MEM_new_array_zeroed<float[3]>(area, "staged mirrored layer");
          if (destination.disps) std::memcpy(staged[g].data, destination.disps, size_t(area)*sizeof(float3));
        }
        copy_v3_v3(staged[g].data[i], value);
        changed++;
      }
    }
  }
  if (!changed) return fail("No unmasked destination detail to change");
  /* Publication cannot fail: allocations and all correspondence checks finished above. */
  weight_hot_delta_cache_invalidate(layer);
  payload_index_invalidate(layer);
  for (const int g : IndexRange(layer->grids_num)) {
    if (!staged[g].data) continue;
    std::swap(staged[g].data, layer->disps[g].disps);
    layer->disps[g].level = layer->level;
    layer->disps[g].totdisp = area;
  }
  return true;
}

/* Base transfer stages cage, native surface and tangent rebasing of retained detail. */
bool symmetrize_multires_base(Object &object,
                             const float3 &origin,
                             const float3 &normal,
                             const float source_sign,
                             const float tolerance,
                             const bool respect_mask,
                             std::string &error)
{
  auto fail = [&](const char *message) { error = message; return false; };
  const auto finite = [](const float3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
  Mesh *mesh = BKE_mesh_from_object(&object);
  auto *mmd = reinterpret_cast<MultiresModifierData *>(BKE_modifiers_findby_type(&object, eModifierType_Multires));
  if (!mesh || !mmd || mesh->key || mesh->id.us > 1 || mesh->id.lib || object.id.lib || mmd->totlvl < 1 || mmd->totlvl > 13 ||
      mmd->sculpt_displacement_enabled || mmd->sculpt_shrinkwrap_enabled) {
    return fail("Base preview requires subdivided Multires without shared data, shape keys or live displacement/shrinkwrap");
  }
  if (!finite(origin) || !finite(normal) || std::abs(math::length(normal)-1.0f)>1e-4f ||
      !std::isfinite(tolerance) || tolerance <= 0) return fail("Invalid symmetry plane");
  const int level=mmd->totlvl, side=subdiv::grid_size_from_level(level), area=side*side;
  const MDisps *native=static_cast<const MDisps *>(CustomData_get_layer(&mesh->corner_data,CD_MDISPS));
  if (!native) return fail("Native Multires grids are missing");
  std::vector<MDisps *> payloads;
  payloads.push_back(const_cast<MDisps *>(native)); /* Read only until publication. */
  std::vector<MultiresSculptLayer *> layers;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (layer.base_deltas) {
      if (layer.base_verts_num != mesh->verts_num) return fail("Incompatible L0 layer vertex count");
      for (int i=0;i<layer.base_verts_num;i++) {
        if (float3(layer.base_deltas[i]) != float3(0.0f)) {
          return fail("Base preview: non-zero L0 layer offsets require a separate rebase; no changes applied");
        }
      }
    }
    if (!layer.disps) continue;
    if (layer.level != level || layer.grids_num != mesh->corners_num) return fail("Incompatible retained layer grids");
    payloads.push_back(layer.disps); layers.push_back(&layer);
  }
  if (int64_t(payloads.size())*mesh->corners_num*area*sizeof(float3)>512LL*1024*1024) {
    return fail("Base preview transaction exceeds 512 MiB");
  }
  for (const MDisps *payload : payloads) {
    for (int g=0;g<mesh->corners_num;g++) {
      if (payload[g].disps && (payload[g].level!=level || payload[g].totdisp!=area)) return fail("Incompatible base/layer grid metadata");
    }
  }
  const auto reflect = [&](const float3 &p) { return p-2.0f*normal*math::dot(p-origin,normal); };
  const Span<float3> positions=mesh->vert_positions();
  for (const float3 &p : positions) { if (!finite(p)) return fail("Non-finite cage coordinate"); }
  KDTree<float3> *tree = kdtree_new<float3>(positions.size());
  for (const int i : positions.index_range()) kdtree_insert(tree, i, positions[i]);
  kdtree_balance(tree);
  Array<int> vertex_map(positions.size(), -1);
  bool mapping_ok = true;
  for (const int i : positions.index_range()) {
    KDTreeNearest<float3> nearest[2];
    const int count = kdtree_find_nearest_n(tree, reflect(positions[i]), nearest, 2);
    if (!count || nearest[0].dist > tolerance ||
        (count > 1 && nearest[1].dist - nearest[0].dist < 1e-7f)) {
      mapping_ok = false;
      break;
    }
    vertex_map[i] = nearest[0].index;
  }
  kdtree_free(tree);
  if (mapping_ok) {
    for (const int i : positions.index_range()) {
      if (vertex_map[vertex_map[i]] != i) mapping_ok = false;
    }
  }
  if (!mapping_ok) return fail("Cage vertices have no unique reciprocal mirror correspondence");
  const OffsetIndices faces = mesh->faces();
  const Span<int> verts = mesh->corner_verts();
  std::map<std::vector<int>, int> lookup;
  for (const int f : faces.index_range()) {
    std::vector<int> key;
    for (const int c : faces[f]) key.push_back(verts[c]);
    std::sort(key.begin(), key.end());
    if (!lookup.emplace(key, f).second) return fail("Duplicate cage faces are ambiguous");
  }
  Array<int> corner_map(mesh->corners_num, -1);
  for (const int f : faces.index_range()) {
    const IndexRange face = faces[f];
    std::vector<int> key;
    for (const int c : face) key.push_back(vertex_map[verts[c]]);
    std::sort(key.begin(), key.end());
    auto found = lookup.find(key);
    if (found == lookup.end()) return fail("Cage topology is not mirror compatible");
    const IndexRange target = faces[found->second];
    for (const int j : IndexRange(face.size())) {
      const int c = face[j], next = face[(j + 1) % face.size()];
      for (const int k : IndexRange(target.size())) {
        if (verts[target[k]] == vertex_map[verts[c]] &&
            verts[target[(k + target.size() - 1) % target.size()]] == vertex_map[verts[next]]) {
          corner_map[c] = target[k];
          break;
        }
      }
      if (corner_map[c] < 0) return fail("Mirrored face winding does not match");
    }
  }
  for (const int c : corner_map.index_range()) {
    if (corner_map[corner_map[c]] != c) return fail("Non-reciprocal corner mapping");
  }

  const GridPaintMask *masks=respect_mask ? static_cast<const GridPaintMask *>(CustomData_get_layer(&mesh->corner_data,CD_GRID_PAINT_MASK)) : nullptr;
  const VArray<float> point_masks=mesh->attributes().lookup_or_default<float>(".sculpt_mask",AttrDomain::Point,0.0f).varray;
  const auto mask_at = [&](int g,float u,float v) {
    if (!masks || !masks[g].data) return 0.0f;
    if (masks[g].level<1 || masks[g].level>13) return 1.0f;
    const int n=subdiv::grid_size_from_level(masks[g].level);
    const float x=u*(n-1), y=v*(n-1);const int x0=int(x),y0=int(y),x1=std::min(x0+1,n-1),y1=std::min(y0+1,n-1);
    const float value=math::interpolate(math::interpolate(masks[g].data[y0*n+x0],masks[g].data[y0*n+x1],x-x0),math::interpolate(masks[g].data[y1*n+x0],masks[g].data[y1*n+x1],x-x0),y-y0);
    return std::isfinite(value) ? std::clamp(value,0.0f,1.0f):1.0f;
  };
  Array<float> vertex_mask(mesh->verts_num,0.0f);
  if (respect_mask) {
    for (const int i:positions.index_range()) vertex_mask[i]=std::clamp(point_masks[i],0.0f,1.0f);
    for (const int c:verts.index_range()) vertex_mask[verts[c]]=std::max(vertex_mask[verts[c]],mask_at(c,1,1));
  }
  if (respect_mask && !masks) {
    for (const int i:positions.index_range()) {
      if (point_masks[i]>0) return fail("Commit the point mask before transferring the Multires base");
    }
  }
  bool fully_masked=respect_mask;
  if (respect_mask) {
    for (const int i:positions.index_range()) fully_masked &= vertex_mask[i]>=1.0f;
    if (masks) {
      for (int g=0;g<mesh->corners_num && fully_masked;g++) {
        if (!masks[g].data || masks[g].level<1 || masks[g].level>13) { fully_masked=false;break; }
        const int n=subdiv::grid_size_from_level(masks[g].level);
        for(int i=0;i<n*n;i++) fully_masked &= masks[g].data[i]>=1.0f;
      }
    }
    else fully_masked=false;
  }
  if(fully_masked)return fail("Destination base is fully masked; nothing changed");
  Array<float3> new_positions(positions);
  for (const int i:positions.index_range()) {
    if (math::dot(positions[i]-origin,normal)*source_sign < -1e-7f) {
      new_positions[i]=math::interpolate(positions[i],reflect(positions[vertex_map[i]]),1.0f-vertex_mask[i]);
      if (!finite(new_positions[i])) return fail("Non-finite cage coordinate");
    }
  }
  bool cage_changed=false;for(const int i:positions.index_range())cage_changed|=positions[i]!=new_positions[i];
  struct Evaluator {
    Mesh *cage=nullptr;
    std::unique_ptr<SubdivCCG> ccg;
    MultiresReshapeContext context{};
    bool initialized=false;
    ~Evaluator() {
      if (initialized) multires_reshape_context_free(&context);
      ccg.reset();if(cage)BKE_id_free(nullptr,cage);
    }
    bool init(Mesh &mesh,MultiresModifierData &mmd,const Span<float3> positions) {
      cage=BKE_mesh_copy_for_eval(mesh);
      cage->vert_positions_for_write().copy_from(positions);cage->tag_positions_changed();
      subdiv::Settings settings;BKE_multires_subdiv_settings_init(&settings,&mmd);
      subdiv::Subdiv *subdiv=subdiv::new_from_mesh(&settings,cage);
      if(!subdiv)return false;
      if(!subdiv::eval_begin_from_mesh(subdiv,cage,subdiv::SUBDIV_EVALUATOR_TYPE_CPU)){subdiv::free(subdiv);return false;}
      SubdivToCCGSettings ccg_settings{};ccg_settings.resolution=3;
      ccg=BKE_subdiv_to_ccg(*subdiv,ccg_settings,*cage,nullptr);
      if(!ccg)return false;
      initialized=true;
      return multires_reshape_context_create_from_ccg_evaluation(&context,ccg.get(),cage,mmd.totlvl);
    }
    void eval(int grid,float u,float v,float3 &p,float3x3 &basis) {
      const GridCoord coord{grid,u,v};multires_reshape_evaluate_base_mesh_limit_at_grid(&context,&coord,p,basis);
    }
  } old_eval,new_eval;
  if(!old_eval.init(*mesh,*mmd,positions) || !new_eval.init(*mesh,*mmd,new_positions))return fail("Cannot initialize base surface evaluators");
  struct Payload { float(*data)[3]=nullptr;~Payload(){if(data)MEM_delete(data);} };
  const int grids=mesh->corners_num;
  std::unique_ptr<Payload[]> staged(new Payload[payloads.size()*grids]);
  int changed=0;const float inv=1.0f/(side-1);
  for(int g=0;g<grids;g++) {
    for(int y=0;y<side;y++)for(int x=0;x<side;x++) {
      const float u=x*inv,v=y*inv;const int i=y*side+x;
      float3 old_p,new_p;float3x3 old_t,new_t;
      old_eval.eval(g,u,v,old_p,old_t);new_eval.eval(g,u,v,new_p,new_t);
      if(std::abs(math::determinant(new_t))<1e-10f)return fail("Degenerate new cage tangent frame");
      const float3x3 inverse=math::invert(new_t);
      for(int k=0;k<int(payloads.size());k++) {
        const MDisps &grid=payloads[k][g];
        const float3 old_value=grid.disps?float3(grid.disps[i]):float3(0.0f);
        if(k>0 && (!grid.disps || !cage_changed))continue;
        if(k==0 && !cage_changed && (math::dot(old_p-origin,normal)*source_sign>=-1e-7f || mask_at(g,u,v)>=1.0f))continue;
        float3 value;
        if(k==0) {
          const float3 old_surface=old_p+old_t*old_value;
          float3 desired=old_surface;
          if(math::dot(old_p-origin,normal)*source_sign < -1e-7f) {
            const int source_grid=corner_map[g];float3 sp;float3x3 st;
            old_eval.eval(source_grid,v,u,sp,st);
            const MDisps &source=native[source_grid];
            const float3 source_value=source.disps?float3(source.disps[x*side+y]):float3(0.0f);
            desired=math::interpolate(old_surface,reflect(sp+st*source_value),1.0f-mask_at(g,u,v));
          }
          value=inverse*(desired-new_p);
        }
        else value=inverse*(old_t*old_value);
        if(!finite(value))return fail("Non-finite rebased displacement; no base changes applied");
        if(value==old_value)continue;
        Payload &stage=staged[k*grids+g];
        if(!stage.data){stage.data=MEM_new_array_zeroed<float[3]>(area,"staged symmetrized base");if(grid.disps)std::memcpy(stage.data,grid.disps,size_t(area)*sizeof(float3));}
        copy_v3_v3(stage.data[i],value);changed++;
      }
    }
  }

  if(!changed && !cage_changed)return fail("No unmasked base changes");
  /* All expensive work and validation have finished. Publish one transaction. */
  MDisps *native_write=static_cast<MDisps *>(CustomData_get_layer_for_write(&mesh->corner_data,CD_MDISPS,grids));
  if(!native_write)return fail("Cannot acquire writable native grids");
  payloads[0]=native_write;
  for(int k=0;k<int(payloads.size());k++)for(int g=0;g<grids;g++) {
    Payload &stage=staged[k*grids+g];if(!stage.data)continue;
    std::swap(stage.data,payloads[k][g].disps);payloads[k][g].level=level;payloads[k][g].totdisp=area;
  }
  mesh->vert_positions_for_write().copy_from(new_positions);mesh->tag_positions_changed();
  weight_hot_delta_cache_invalidate();payload_index_invalidate();
  return true;
}
