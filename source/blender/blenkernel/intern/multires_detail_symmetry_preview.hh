/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included inside multires_layers: transactional, topology-preserving detail transfer. */

static bool build_topological_mirror_map(const Mesh &mesh,
                                         const float3 &origin,
                                         const float3 &normal,
                                         const float tolerance,
                                         Array<int> &vertex_map,
                                         Array<int> &corner_map,
                                         std::string &error)
{
  const Span<float3> positions = mesh.vert_positions();
  const OffsetIndices faces = mesh.faces();
  const Span<int> verts = mesh.corner_verts();
  const auto fail = [&](const char *message) { error = message; return false; };
  const auto reflect = [&](const float3 &p) {
    return p - 2.0f * normal * math::dot(p - origin, normal);
  };

  /* Geometry supplies only trustworthy seeds. Connectivity propagates each seed over the
   * component, so an intentionally sculpted side does not need to remain geometrically equal. */
  KDTree<float3> *tree = kdtree_new<float3>(positions.size());
  for (const int i : positions.index_range()) {
    kdtree_insert(tree, i, positions[i]);
  }
  kdtree_balance(tree);
  Array<int> nearest(positions.size(), -1);
  Array<int> relaxed_nearest(positions.size(), -1);
  for (const int i : positions.index_range()) {
    KDTreeNearest<float3> hits[2];
    const int count = kdtree_find_nearest_n(tree, reflect(positions[i]), hits, 2);
    if (count && (count < 2 || hits[1].dist - hits[0].dist >= 1.0e-7f))
    {
      relaxed_nearest[i] = hits[0].index;
      if (hits[0].dist <= tolerance) {
        nearest[i] = hits[0].index;
      }
    }
  }
  kdtree_free(tree);
  for (Array<int> *candidates : {&nearest, &relaxed_nearest}) {
    for (const int i : candidates->index_range()) {
      if ((*candidates)[i] >= 0 && (*candidates)[(*candidates)[i]] != i) {
        (*candidates)[i] = -1;
      }
    }
  }

  std::map<std::vector<int>, int> face_lookup;
  std::map<std::pair<int, int>, std::vector<int>> edge_faces;
  for (const int f : faces.index_range()) {
    std::vector<int> key;
    for (const int c : faces[f]) {
      key.push_back(verts[c]);
      const int next = faces[f][(c - faces[f].start() + 1) % faces[f].size()];
      edge_faces[std::minmax(verts[c], verts[next])].push_back(f);
    }
    std::sort(key.begin(), key.end());
    if (!face_lookup.emplace(key, f).second) {
      return fail("Duplicate cage faces are ambiguous");
    }
  }
  for (const auto &item : edge_faces) {
    if (item.second.size() > 2) {
      return fail("Non-manifold cage edges are ambiguous");
    }
  }

  vertex_map.fill(-1);
  corner_map.fill(-1);
  Array<int> face_map(faces.size(), -1);
  std::vector<std::pair<int, int>> queue;
  size_t queue_index = 0;

  const auto assign_vertex = [&](const int a, const int b) {
    if ((vertex_map[a] >= 0 && vertex_map[a] != b) ||
        (vertex_map[b] >= 0 && vertex_map[b] != a))
    {
      return false;
    }
    vertex_map[a] = b;
    vertex_map[b] = a;
    return true;
  };
  const auto assign_face = [&](const int source_face,
                               const int target_face,
                               const int source_start,
                               const int target_start) {
    const IndexRange source = faces[source_face];
    const IndexRange target = faces[target_face];
    if (source.size() != target.size() ||
        (face_map[source_face] >= 0 && face_map[source_face] != target_face) ||
        (face_map[target_face] >= 0 && face_map[target_face] != source_face))
    {
      return false;
    }
    face_map[source_face] = target_face;
    face_map[target_face] = source_face;
    for (const int step : IndexRange(source.size())) {
      const int sc = source[(source_start + step) % source.size()];
      const int tc = target[(target_start + target.size() - step) % target.size()];
      if (!assign_vertex(verts[sc], verts[tc]) ||
          (corner_map[sc] >= 0 && corner_map[sc] != tc) ||
          (corner_map[tc] >= 0 && corner_map[tc] != sc))
      {
        return false;
      }
      corner_map[sc] = tc;
      corner_map[tc] = sc;
    }
    queue.emplace_back(source_face, target_face);
    return true;
  };

  const auto propagate = [&]() {
    while (queue_index < queue.size()) {
      const auto [source_face, target_face] = queue[queue_index++];
      const IndexRange source = faces[source_face];
      for (const int j : IndexRange(source.size())) {
        const int c = source[j];
        const int next = source[(j + 1) % source.size()];
        const int a = verts[c], b = verts[next];
        const std::vector<int> &source_adjacent = edge_faces.at(std::minmax(a, b));
        const std::vector<int> &target_adjacent = edge_faces.at(
            std::minmax(vertex_map[a], vertex_map[b]));
        if (source_adjacent.size() != target_adjacent.size()) {
          return false;
        }
        if (source_adjacent.size() == 1) {
          continue;
        }
        const int source_other = source_adjacent[0] == source_face ? source_adjacent[1] :
                                                                      source_adjacent[0];
        const int target_other = target_adjacent[0] == target_face ? target_adjacent[1] :
                                                                      target_adjacent[0];
        if (face_map[source_other] >= 0) {
          if (face_map[source_other] != target_other) {
            return false;
          }
          continue;
        }
        const IndexRange source_next = faces[source_other];
        const IndexRange target_next = faces[target_other];
        int source_start = -1, target_start = -1;
        for (const int k : IndexRange(source_next.size())) {
          if (verts[source_next[k]] == b &&
              verts[source_next[(k + 1) % source_next.size()]] == a)
          {
            source_start = k;
            break;
          }
        }
        for (const int k : IndexRange(target_next.size())) {
          if (verts[target_next[k]] == vertex_map[b] &&
              verts[target_next[(k + target_next.size() - 1) % target_next.size()]] ==
                  vertex_map[a])
          {
            target_start = k;
            break;
          }
        }
        if (source_start < 0 || target_start < 0 ||
            !assign_face(source_other, target_other, source_start, target_start))
        {
          return false;
        }
      }
    }
    return true;
  };

  const auto seed_components = [&](const Span<int> candidates) {
    bool progress = true;
    while (progress) {
      progress = false;
      for (const int f : faces.index_range()) {
        if (face_map[f] >= 0) {
          continue;
        }
        const IndexRange source = faces[f];
        std::vector<int> key;
        bool complete = true;
        for (const int c : source) {
          const int target_vertex = candidates[verts[c]];
          if (target_vertex < 0) {
            complete = false;
            break;
          }
          key.push_back(target_vertex);
        }
        if (!complete) {
          continue;
        }
        std::sort(key.begin(), key.end());
        const auto found = face_lookup.find(key);
        if (found == face_lookup.end()) {
          continue;
        }
        const IndexRange target = faces[found->second];
        int target_start = -1;
        for (const int k : IndexRange(target.size())) {
          if (verts[target[k]] == candidates[verts[source[0]]] &&
              verts[target[(k + target.size() - 1) % target.size()]] ==
                  candidates[verts[source[1]]])
          {
            target_start = k;
            break;
          }
        }
        if (target_start < 0 || !assign_face(f, found->second, 0, target_start) ||
            !propagate())
        {
          return false;
        }
        progress = true;
      }
    }
    return true;
  };
  /* Prefer strict geometric seeds. If a whole component was sculpted away from its mirror,
   * retry with reciprocal nearest candidates at any distance. Connectivity remains the final
   * authority and must still validate every face, edge, corner and vertex before publication. */
  if (!seed_components(nearest) || !seed_components(relaxed_nearest)) {
    return fail("Cage topology has an ambiguous mirror correspondence");
  }

  /* Loose vertices cannot be propagated through faces; accept only strict reciprocal seeds. */
  for (const int i : vertex_map.index_range()) {
    if (vertex_map[i] < 0 && nearest[i] >= 0 && vertex_map[nearest[i]] < 0) {
      if (!assign_vertex(i, nearest[i])) {
        return fail("Ambiguous loose vertex mirror correspondence");
      }
    }
  }
  for (const int f : face_map.index_range()) {
    if (face_map[f] < 0) {
      return fail("Cage topology cannot be seeded from the selected symmetry plane");
    }
  }
  for (const int i : vertex_map.index_range()) {
    if (vertex_map[i] < 0 || vertex_map[vertex_map[i]] != i) {
      return fail("Cage vertices have no reciprocal topological mirror correspondence");
    }
  }
  for (const int c : corner_map.index_range()) {
    if (corner_map[c] < 0 || corner_map[corner_map[c]] != c) {
      return fail("Non-reciprocal corner mapping");
    }
  }
  return true;
}

bool symmetrize_active_detail(Object &object,
                             const float3 &origin,
                             const float3 &normal,
                             const float source_sign,
                             const float tolerance,
                             const bool respect_mask,
                             std::string &error)
{
  const bool trace = []() {
    const char *value = BLI_getenv("BLENDER_LAYER_RESP_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  const double total_start = trace ? BLI_time_now_seconds() : 0.0;
  double topology_ms = 0.0;
  double occupancy_ms = 0.0;
  double evaluator_ms = 0.0;
  double solve_ms = 0.0;
  double publish_ms = 0.0;
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
  for (const MDisps &grid : Span(layer->disps, layer->grids_num)) {
    if (grid.disps && (grid.level != layer->level || grid.totdisp != area)) {
      return fail("Incompatible layer grid metadata");
    }
  }
  /* Older files can contain allocated zero grids. Pointer presence alone therefore makes a
   * localized layer look dense and causes hundreds of millions of unnecessary limit-surface
   * evaluations. Build an exact transient occupancy map first; scanning floats is far cheaper
   * than evaluating frames and does not approximate or discard any detail. */
  const double occupancy_start = trace ? BLI_time_now_seconds() : 0.0;
  Array<bool> occupied_grid(layer->grids_num, false);
  threading::parallel_for(IndexRange(layer->grids_num), 256, [&](const IndexRange range) {
    for (const int g : range) {
      const MDisps &grid = layer->disps[g];
      if (!grid.disps) {
        continue;
      }
      for (const int i : IndexRange(area)) {
        const float3 value(grid.disps[i]);
        if (value.x != 0.0f || value.y != 0.0f || value.z != 0.0f) {
          occupied_grid[g] = true;
          break;
        }
      }
    }
  });
  int occupied_grids = 0;
  for (const bool occupied : occupied_grid) {
    occupied_grids += int(occupied);
  }
  if (trace) {
    occupancy_ms = (BLI_time_now_seconds() - occupancy_start) * 1000.0;
  }
  if (occupied_grids == 0) {
    return fail("Active layer has no detail to symmetrize");
  }
  const auto reflect = [&](const float3 &p) {
    return p - 2.0f * normal * math::dot(p - origin, normal);
  };
  const auto reflect_vector = [&](const float3 &v) {
    return v - 2.0f * normal * math::dot(v, normal);
  };
  const Span<float3> positions = mesh->vert_positions();
  for (const float3 &p : positions) { if (!finite(p)) return fail("Non-finite cage coordinate"); }
  Array<int> vertex_map(positions.size(), -1);
  const OffsetIndices faces = mesh->faces();
  const Span<int> verts = mesh->corner_verts();
  Array<int> corner_map(mesh->corners_num, -1);
  const double topology_start = trace ? BLI_time_now_seconds() : 0.0;
  if (!build_topological_mirror_map(
          *mesh, origin, normal, tolerance, vertex_map, corner_map, error)) return false;
  if (trace) {
    topology_ms = (BLI_time_now_seconds() - topology_start) * 1000.0;
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
  const double evaluator_start = trace ? BLI_time_now_seconds() : 0.0;
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
  if (trace) {
    evaluator_ms = (BLI_time_now_seconds() - evaluator_start) * 1000.0;
  }
  struct ContextOwner { MultiresReshapeContext *value; ~ContextOwner() { multires_reshape_context_free(value); } } context_owner{&context};
  struct Payload {
    float (*data)[3] = nullptr;
    ~Payload() { if (data) MEM_delete(data); }
  };
  std::unique_ptr<Payload[]> staged(new Payload[layer->grids_num]);
  Array<int> changed_per_grid(layer->grids_num, 0);
  Array<int> failure_per_grid(layer->grids_num, 0);
  Array<bool> fully_masked_grid(layer->grids_num, false);
  if (respect_mask && masks) {
    threading::parallel_for(IndexRange(layer->grids_num), 256, [&](const IndexRange range) {
      for (const int g : range) {
        if (!masks[g].data || masks[g].level < 1 || masks[g].level > 13) {
          continue;
        }
        const int mask_side = subdiv::grid_size_from_level(masks[g].level);
        bool fully_masked = true;
        for (int i = 0; i < mask_side * mask_side && fully_masked; i++) {
          fully_masked = masks[g].data[i] >= 1.0f;
        }
        fully_masked_grid[g] = fully_masked;
      }
    });
  }
  int reciprocal_pairs = 0;
  int active_pairs = 0;
  int self_grids = 0;
  for (const int g : IndexRange(layer->grids_num)) {
    const int source_grid = corner_map[g];
    if (source_grid < g) {
      continue;
    }
    reciprocal_pairs++;
    self_grids += int(source_grid == g);
    if ((occupied_grid[g] || occupied_grid[source_grid]) &&
        !(fully_masked_grid[g] && fully_masked_grid[source_grid]))
    {
      active_pairs++;
    }
  }
  const float inv = 1.0f / (side - 1);
  const double solve_start = trace ? BLI_time_now_seconds() : 0.0;
  threading::parallel_for(IndexRange(layer->grids_num), 1, [&](const IndexRange range) {
    for (const int g : range) {
      const int source_grid = corner_map[g];
      /* A reciprocal pair is owned by its lowest grid index. This avoids evaluating the target
       * side again after it was already evaluated as the source of its mirror partner. */
      if (source_grid < g) {
        continue;
      }
      const MDisps &a = layer->disps[g];
      const MDisps &b = layer->disps[source_grid];
      if ((!occupied_grid[g] && !occupied_grid[source_grid]) ||
          (fully_masked_grid[g] && fully_masked_grid[source_grid]))
      {
        continue;
      }

      const auto apply_destination = [&](const int destination_grid,
                                         const int destination_index,
                                         const float u,
                                         const float v,
                                         const float3x3 &target_basis,
                                         const MDisps &destination,
                                         const int source_index,
                                         const float3x3 &source_basis,
                                         const MDisps &source) {
        if (fully_masked_grid[destination_grid]) {
          return;
        }
        const float weight = 1.0f - mask_at(destination_grid, u, v);
        if (weight == 0.0f) {
          return;
        }
        if (std::abs(math::determinant(target_basis)) < 1e-10f) {
          failure_per_grid[destination_grid] = 1;
          return;
        }
        float3 desired(0.0f);
        if (source.disps) {
          if (std::abs(math::determinant(source_basis)) < 1e-10f) {
            failure_per_grid[destination_grid] = 2;
            return;
          }
          desired = math::invert(target_basis) *
                    reflect_vector(source_basis * float3(source.disps[source_index]));
        }
        const float3 old = destination.disps ? float3(destination.disps[destination_index]) :
                                               float3(0.0f);
        const float3 value = math::interpolate(old, desired, weight);
        if (!finite(value)) {
          failure_per_grid[destination_grid] = 3;
          return;
        }
        if (value == old) {
          return;
        }
        if (!staged[destination_grid].data) {
          staged[destination_grid].data = MEM_new_array_zeroed<float[3]>(
              area, "staged mirrored layer");
          if (destination.disps) {
            std::memcpy(staged[destination_grid].data,
                        destination.disps,
                        size_t(area) * sizeof(float3));
          }
        }
        copy_v3_v3(staged[destination_grid].data[destination_index], value);
        changed_per_grid[destination_grid]++;
      };

      /* A grid on the symmetry plane can map to itself. Keep the original one-direction walk
       * for that uncommon case to avoid visiting transposed samples twice. */
      if (source_grid == g) {
        for (int y = 0; y < side && failure_per_grid[g] == 0; y++) {
          for (int x = 0; x < side; x++) {
            const float u = x * inv, v = y * inv;
            float3 p, source_p;
            float3x3 target_basis, source_basis;
            const GridCoord target_coord{g, u, v};
            const GridCoord source_coord{g, v, u};
            multires_reshape_evaluate_base_mesh_limit_at_grid(
                &context, &target_coord, p, target_basis);
            if (math::dot(p - origin, normal) * source_sign >= -1e-7f) {
              continue;
            }
            multires_reshape_evaluate_base_mesh_limit_at_grid(
                &context, &source_coord, source_p, source_basis);
            apply_destination(g,
                              y * side + x,
                              u,
                              v,
                              target_basis,
                              a,
                              x * side + y,
                              source_basis,
                              a);
          }
        }
        continue;
      }

      for (int y = 0;
           y < side && failure_per_grid[g] == 0 && failure_per_grid[source_grid] == 0;
           y++)
      {
        for (int x = 0; x < side; x++) {
          const float u = x * inv, v = y * inv;
          const int a_index = y * side + x;
          const int b_index = x * side + y;
          float3 a_position, b_position;
          float3x3 a_basis, b_basis;
          const GridCoord a_coord{g, u, v};
          const GridCoord b_coord{source_grid, v, u};
          multires_reshape_evaluate_base_mesh_limit_at_grid(
              &context, &a_coord, a_position, a_basis);
          multires_reshape_evaluate_base_mesh_limit_at_grid(
              &context, &b_coord, b_position, b_basis);
          if (math::dot(a_position - origin, normal) * source_sign < -1e-7f) {
            apply_destination(
                g, a_index, u, v, a_basis, a, b_index, b_basis, b);
          }
          if (math::dot(b_position - origin, normal) * source_sign < -1e-7f) {
            apply_destination(source_grid,
                              b_index,
                              v,
                              u,
                              b_basis,
                              b,
                              a_index,
                              a_basis,
                              a);
          }
        }
      }
    }
  });
  if (trace) {
    solve_ms = (BLI_time_now_seconds() - solve_start) * 1000.0;
  }
  const double publish_start = trace ? BLI_time_now_seconds() : 0.0;
  int changed = 0;
  for (const int g : IndexRange(layer->grids_num)) {
    if (failure_per_grid[g] == 1) return fail("Degenerate destination surface frame");
    if (failure_per_grid[g] == 2) return fail("Degenerate source surface frame");
    if (failure_per_grid[g] == 3) return fail("Non-finite layer displacement; nothing transferred");
    changed += changed_per_grid[g];
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
  if (trace) {
    publish_ms = (BLI_time_now_seconds() - publish_start) * 1000.0;
    const int64_t pair_samples = int64_t(active_pairs) * int64_t(area);
    std::printf(
        "[SYMMETRY_DETAIL_PROFILE] grids=%d occupied=%d pairs=%d active_pairs=%d self=%d area=%d "
        "pair_samples=%lld changed=%d occupancy=%.3fms topology=%.3fms evaluator=%.3fms solve=%.3fms "
        "publish=%.3fms total=%.3fms\n",
        layer->grids_num,
        occupied_grids,
        reciprocal_pairs,
        active_pairs,
        self_grids,
        area,
        static_cast<long long>(pair_samples),
        changed,
        occupancy_ms,
        topology_ms,
        evaluator_ms,
        solve_ms,
        publish_ms,
        (BLI_time_now_seconds() - total_start) * 1000.0);
    std::fflush(stdout);
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
  for (const MDisps *payload : payloads) {
    for (int g=0;g<mesh->corners_num;g++) {
      if (payload[g].disps && (payload[g].level!=level || payload[g].totdisp!=area)) return fail("Incompatible base/layer grid metadata");
    }
  }
  const auto reflect = [&](const float3 &p) { return p-2.0f*normal*math::dot(p-origin,normal); };
  const Span<float3> positions=mesh->vert_positions();
  for (const float3 &p : positions) { if (!finite(p)) return fail("Non-finite cage coordinate"); }
  Array<int> vertex_map(positions.size(), -1);
  const OffsetIndices faces = mesh->faces();
  const Span<int> verts = mesh->corner_verts();
  Array<int> corner_map(mesh->corners_num, -1);
  if (!build_topological_mirror_map(
          *mesh, origin, normal, tolerance, vertex_map, corner_map, error)) return false;

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
  Array<int> changed_per_grid(grids,0),failure_per_grid(grids,0);
  const float inv=1.0f/(side-1);
  threading::parallel_for(IndexRange(grids),1,[&](const IndexRange range){
   for(const int g:range) {
    if(respect_mask && masks && masks[g].data && masks[g].level>=1 && masks[g].level<=13) {
      const int mask_side=subdiv::grid_size_from_level(masks[g].level);bool fully_masked=true;
      for(int i=0;i<mask_side*mask_side && fully_masked;i++)fully_masked=masks[g].data[i]>=1.0f;
      if(fully_masked)continue;
    }
    for(int y=0;y<side && failure_per_grid[g]==0;y++)for(int x=0;x<side;x++) {
      const float u=x*inv,v=y*inv;const int i=y*side+x;
      float3 old_p,new_p;float3x3 old_t,new_t;
      old_eval.eval(g,u,v,old_p,old_t);new_eval.eval(g,u,v,new_p,new_t);
      if(std::abs(math::determinant(new_t))<1e-10f){failure_per_grid[g]=1;break;}
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
        if(!finite(value)){failure_per_grid[g]=2;break;}
        if(value==old_value)continue;
        Payload &stage=staged[k*grids+g];
        if(!stage.data){stage.data=MEM_new_array_zeroed<float[3]>(area,"staged symmetrized base");if(grid.disps)std::memcpy(stage.data,grid.disps,size_t(area)*sizeof(float3));}
        copy_v3_v3(stage.data[i],value);changed_per_grid[g]++;
      }
    }
   }
  });
  int changed=0;for(int g=0;g<grids;g++){
    if(failure_per_grid[g]==1)return fail("Degenerate new cage tangent frame");
    if(failure_per_grid[g]==2)return fail("Non-finite rebased displacement; no base changes applied");
    changed+=changed_per_grid[g];
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
