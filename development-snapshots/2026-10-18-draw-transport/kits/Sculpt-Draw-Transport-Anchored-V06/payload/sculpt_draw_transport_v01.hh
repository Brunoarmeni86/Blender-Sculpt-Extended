/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included inside blender::ed::sculpt_paint, after the native Undo/cursor helpers. */
namespace draw_transport_v01 {
struct Triangle {
  std::array<float3, 3> co, delta, normal;
};
struct ReferenceSample {
  int index = -1;
  float3 position = float3(0), normal = float3(0);
  float mask_factor = 1.0f;
  bool hidden = false;
};
struct SavedPosition { int index; float3 position; };
struct State {
  std::vector<Triangle> triangles;
  BVHTree *tree = nullptr;
  SubdivCCG *ccg = nullptr;
  float3 center = float3(0), normal = float3(0, 0, 1);
  float radius = 0, support = 0;
  float2 mouse_offset = float2(0);
  float2 target_mouse = float2(0);
  float angle = 0.0f, rotation_base = 0.0f, rotation_mouse_start = 0.0f;
  bool rotating = false, scaling = false, strength_adjust = false;
  float footprint_scale = 1.0f, imprint_strength = 1.0f;
  float scale_base = 1.0f, scale_mouse_start = 1.0f;
  float strength_base = 1.0f, strength_mouse_start = 0.0f;
  struct Pending { int index; float3 delta; };
  std::vector<Pending> pending;
  std::vector<int> write_grids;
  std::vector<float3> write_translations;
  bool adjusting() const { return rotating || scaling || strength_adjust; }
  float initial_brush_rotation = 0.0f, initial_mask_rotation = 0.0f;
  const char *failure_reason = nullptr;
  int64_t capture_samples = 0;
  bool reference_path = false, parallel = true, profile = false, coherent = true;
  bool target_cached = false, region_cached = false, mapping_cached = false;
  float2 cached_mouse = float2(0);
  float3 cached_location = float3(0), cached_normal = float3(0);
  float region_support = 0.0f, mapped_angle = 0.0f, mapped_scale = 1.0f;
  std::vector<int> region_nodes, region_grids, preview_nodes;
  std::vector<ReferenceSample> region_samples;
  std::vector<float3> mapped_deltas;
  std::vector<int> nearest_seeds;
  std::vector<SavedPosition> preview_before;
  int64_t positions_num = 0, profile_frames = 0;
  std::array<double,5> profile_total = {};
  bool placing = false, valid = false;
  ~State() {
    if (profile && profile_frames) {
      std::printf("[DRAW_TRANSPORT_V06_SUMMARY] frames=%lld avg_restore_ms=%.3f avg_surface_ms=%.3f avg_map_ms=%.3f avg_write_ms=%.3f avg_update_ms=%.3f\n",
          static_cast<long long>(profile_frames),profile_total[0]/profile_frames,
          profile_total[1]/profile_frames,profile_total[2]/profile_frames,
          profile_total[3]/profile_frames,profile_total[4]/profile_frames);
      std::fflush(stdout);
    }
    if (tree) { BLI_bvhtree_free(tree); }
  }
};
inline bool finite(const float3 &p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
inline float3 rotate(const float3 &v, const float3 &from, const float3 &to)
{
  const float c = std::clamp(math::dot(from, to), -1.0f, 1.0f);
  if (c > 0.999999f) { return v; }
  if (c < -0.9999f) {
    float3 axis = math::cross(from, float3(1, 0, 0));
    if (math::length_squared(axis) < 1e-8f) { axis = math::cross(from, float3(0, 1, 0)); }
    axis = math::normalize(axis);
    return 2.0f * math::dot(axis, v) * axis - v;
  }
  const float3 axis = math::normalize(math::cross(from, to));
  const float sine = std::sqrt(std::max(0.0f,1.0f-c*c));
  return c*v + sine*math::cross(axis,v) + (1.0f-c)*math::dot(axis,v)*axis;
}
inline float3 rotate_axis(const float3 &v, const float3 &axis, const float angle)
{
  const float cosine = std::cos(angle), sine = std::sin(angle);
  return cosine*v + sine*math::cross(axis,v) + (1.0f-cosine)*math::dot(axis,v)*axis;
}
/* Scale only coordinates tangent to the imprint plane. The normal/depth coordinate stays unchanged. */
inline float3 footprint_inverse(const float3 &relative, const float3 &normal, const float scale)
{
  const float3 depth = normal*math::dot(relative,normal);
  return depth+(relative-depth)/scale;
}
inline float footprint_from_mouse(const float base, const float distance, const float start)
{
  return std::clamp(base*std::max(distance,8.0f)/start,0.05f,8.0f);
}
inline float strength_from_mouse(const float base, const float mouse, const float start)
{
  return std::clamp(base+(mouse-start)*0.01f,0.0f,10.0f);
}
inline void nearest_cb(void *userdata, int index, const float co[3], BVHTreeNearest *nearest)
{
  const State &state = *static_cast<const State *>(userdata);
  const Triangle &tri = state.triangles[index];
  float3 point;
  closest_on_tri_to_point_v3(point, co, tri.co[0], tri.co[1], tri.co[2]);
  const float d2 = math::distance_squared(point, float3(co));
  if (d2 < nearest->dist_sq) {
    nearest->index = index;
    nearest->dist_sq = d2;
    copy_v3_v3(nearest->co, point);
  }
}
inline bool weights(const Triangle &tri, const float3 &p, float3 &w)
{
  const float3 a = tri.co[1]-tri.co[0], b = tri.co[2]-tri.co[0], q = p-tri.co[0];
  const double aa = math::dot(a,a), ab = math::dot(a,b), bb = math::dot(b,b);
  const double aq = math::dot(a,q), bq = math::dot(b,q), det = aa*bb-ab*ab;
  if (det <= 1e-14*std::max(aa,bb)*std::max(aa,bb) || !std::isfinite(det)) { return false; }
  w.y = float((bb*aq-ab*bq)/det); w.z = float((aa*bq-ab*aq)/det); w.x = 1-w.y-w.z;
  return finite(w);
}
inline bool enabled(const Brush &brush)
{
  const char *v = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_V01");
  if (!v || v[0] != '1' || v[1] != '\0') { return false; }
  PointerRNA ptr = RNA_id_pointer_create(const_cast<ID *>(&brush.id));
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "sculpt_draw_transport_v01");
  return prop && RNA_property_boolean_get(&ptr, prop);
}
inline const char *preflight(bContext *C, const Brush &brush, const wmOperator &op)
{
  const SculptSession *ss = CTX_data_active_object(C)->runtime->sculpt_session;
  const MultiresModifierData *mmd = ss ? ss->multires_modifier : nullptr;
  const Object &ob = *CTX_data_active_object(C);
  const Mesh &mesh = *id_cast<Mesh *>(ob.data);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  if (brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_DRAW || brush.stroke_method != BRUSH_STROKE_ANCHORED ||
      brush.falloff_shape != PAINT_FALLOFF_SHAPE_SPHERE || (brush.flag & BRUSH_EDGE_TO_EDGE) ||
      RNA_enum_get(op.ptr, "brush_toggle") != int(BrushSwitchMode::None))
  { return "Draw Transport V06 requires Draw, Anchored, sphere falloff, no Edge to Edge or alternate brush"; }
  if (!ss || ss->bm || !ss->subdiv_ccg || !mmd || ss->cache || ss->filter_cache ||
      ss->subdiv_ccg->level != mmd->totlvl || mmd->sculptlvl != mmd->totlvl ||
      mmd->sculpt_displacement_enabled || mmd->sculpt_shrinkwrap_enabled ||
      (mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh) || ob.modifiers.count() != 1 ||
      mesh.id.us != 1 || mesh.key || ob.id.lib || ob.id.override_library || mesh.id.lib)
  { return "Draw Transport V06 requires local single-user Multires at maximum Sculpt Level, no other modifiers or live deformation"; }
  if (mesh_symmetry_xyz_get(ob) || mesh.radial_symmetry[0] != 1 || mesh.radial_symmetry[1] != 1 ||
      mesh.radial_symmetry[2] != 1 || (sd.paint.symmetry_flags & (PAINT_TILE_X|PAINT_TILE_Y|PAINT_TILE_Z)) ||
      (sd.flags & (SCULPT_LOCK_X|SCULPT_LOCK_Y|SCULPT_LOCK_Z)))
  { return "Draw Transport V06 requires mirror/radial/tile symmetry and axis locks disabled"; }
  if (!BKE_sculptsession_use_pbvh_draw(&ob, CTX_wm_region_view3d(C)))
  { return "Draw Transport V06 requires native PBVH viewport drawing"; }
  if (!(U.uiflag & USER_GLOBALUNDO) || U.undosteps <= 0)
  { return "Draw Transport V06 requires Global Undo"; }
  return nullptr;
}
inline bool fail(State &state, const char *reason)
{
  state.failure_reason = reason;
  std::fprintf(stderr,"[DRAW_TRANSPORT_V06] stopped=%s source_samples=%lld triangles=%zu\n",
               reason,static_cast<long long>(state.capture_samples),state.triangles.size());
  return false;
}
inline bool capture(State &state, Object &ob)
{
  const double capture_start = BLI_time_now_seconds();
  const char *reference = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_REFERENCE");
  const char *serial = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_SERIAL");
  const char *profiling = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_PROFILE");
  const char *coherence = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_NO_COHERENCE");
  state.coherent = !(coherence && coherence[0] == '1');
  state.reference_path = reference && reference[0] == '1';
  state.parallel = !(serial && serial[0] == '1');
  state.profile = profiling && profiling[0] == '1';
  SculptSession &ss = *ob.runtime->sculpt_session;
  if (!ss.cache || !ss.subdiv_ccg || ss.cache->radius <= 1e-8f) { return fail(state,"No active Multires imprint or zero radius"); }
  state.ccg = ss.subdiv_ccg;
  state.positions_num = state.ccg->positions.size();
  state.center = ss.cache->location;
  state.normal = math::normalize(ss.cache->sculpt_normal);
  if (!finite(state.normal) || math::length_squared(state.normal) < .5f) { return fail(state,"Invalid source normal"); }
  state.radius = ss.cache->radius;
  state.initial_brush_rotation = ss.cache->paint->runtime->brush_rotation;
  state.initial_mask_rotation = ss.cache->paint->runtime->brush_rotation_sec;
  const Span<bke::pbvh::GridsNode> nodes = bke::object::pbvh_get(ob)->nodes<bke::pbvh::GridsNode>();
  const int side = state.ccg->grid_size, area = state.ccg->grid_area;
  int64_t samples = 0;
  bool any_delta = false;
  for (const auto &node : nodes) {
    const std::optional<OrigPositionData> original = orig_position_data_lookup_grids(ob, node);
    if (!original) { continue; }
    samples += int64_t(node.grids().size()) * area;
    state.capture_samples = samples;
    if (original->positions.size() != node.grids().size()*area ||
        original->normals.size() != original->positions.size()) { return fail(state,"Undo/source grid layout differs"); }
    if (!state.reference_path) { state.preview_nodes.push_back(int(&node-nodes.data())); }
    int base = 0;
    for (const int grid : node.grids()) {
      if (!state.reference_path) {
        for (int j=0;j<area;j++) {
          const int index=grid*area+j;
          const float3 before=original->positions[base+j];
          const float3 current=state.ccg->positions[index];
          if (before.x!=current.x || before.y!=current.y || before.z!=current.z) {
            state.preview_before.push_back({index,before});
          }
        }
      }
      for (int y = 0; y < side-1; y++) {
        for (int x = 0; x < side-1; x++) {
          const int a = y*side+x, b = a+1, c = a+side, d = c+1;
          for (const std::array<int,3> corners : {std::array<int,3>{a,b,d}, std::array<int,3>{a,d,c}}) {
            Triangle tri;
            bool changed = false;
            for (int j = 0; j < 3; j++) {
              const int local = corners[j];
              tri.co[j] = original->positions[base+local];
              tri.normal[j] = original->normals[base+local];
              tri.delta[j] = state.ccg->positions[grid*area+local]-tri.co[j];
              if (!finite(tri.co[j]) || !finite(tri.delta[j]) || !finite(tri.normal[j])) { return fail(state,"Non-finite imprint coordinates or normals"); }
              changed |= math::length_squared(tri.delta[j]) > 1e-20f;
            }
            if (!changed) { continue; }
            if (math::length_squared(math::cross(tri.co[1]-tri.co[0],tri.co[2]-tri.co[0])) < 1e-24f) { continue; }
            any_delta = true;
            for (int j=0;j<3;j++) { state.support = std::max(state.support, math::distance(tri.co[j],state.center)); }
            /* BVH primitive IDs are signed int; this is an API representability check, not a prototype sample cap. */
            if (state.triangles.size() >= size_t(std::numeric_limits<int>::max())) {
              return fail(state,"BVH primitive IDs cannot represent this footprint");
            }
            state.triangles.push_back(tri);
          }
        }
      }
      base += area;
    }
  }
  if (!any_delta) { return fail(state,"No nonzero displacement in the Anchored imprint"); }
  state.tree = BLI_bvhtree_new(int(state.triangles.size()), 0.0f, 4, 6);
  if (!state.tree) { return fail(state,"Cannot allocate the source BVH"); }
  for (int i = 0; i < int(state.triangles.size()); i++) {
    BLI_bvhtree_insert(state.tree, i, state.triangles[i].co[0], 3);
  }
  const double tree_start = BLI_time_now_seconds();
  BLI_bvhtree_balance(state.tree);
  if (state.profile) {
    std::printf("[DRAW_TRANSPORT_V06_CAPTURE] total_ms=%.3f balance_ms=%.3f triangles=%zu source_samples=%lld sparse_restore_samples=%zu reference=%d parallel=%d\n",
        (BLI_time_now_seconds()-capture_start)*1000.0,(BLI_time_now_seconds()-tree_start)*1000.0,
        state.triangles.size(),static_cast<long long>(samples),state.preview_before.size(),int(state.reference_path),int(state.parallel));
  }
  state.placing = true;
  state.valid = true; /* Initial imprint remains a valid position until first motion. */
  std::printf("[DRAW_TRANSPORT_V06] captured samples=%lld triangles=%zu radius=%.6f\n",
              static_cast<long long>(samples), state.triangles.size(), state.radius);
  std::fflush(stdout);
  return true;
}
inline bool sphere_bounds(const Bounds<float3> &bounds, const float3 &center, const float radius)
{
  float d2 = 0;
  for (int i=0;i<3;i++) {
    const float d = std::max({bounds.min[i]-center[i], 0.0f, center[i]-bounds.max[i]});
    d2 += d*d;
  }
  return d2 <= radius*radius;
}
/* Native Undo originals stay authoritative. Never incrementally move an already moved imprint. */
inline bool move_reference(State &state, bContext *C, ViewContext &vc, const wmEvent &event)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  if (ss.subdiv_ccg != state.ccg || !ss.cache) { return fail(state,"Multires surface changed during positioning"); }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  undo::restore_from_undo_step(depsgraph, sd, ob);
  pbvh.update_bounds(depsgraph, ob);
  bke::pbvh::update_normals(depsgraph, ob, pbvh);
  state.valid = false;
  const float2 event_mouse(float(event.mval[0]),float(event.mval[1]));
  const float2 mouse = state.adjusting() ? state.target_mouse : event_mouse+state.mouse_offset;
  if (state.rotating) {
    const float2 radial = event_mouse-state.target_mouse;
    if (math::length_squared(radial) >= 4.0f) {
      /* Same polar-angle convention as Anchored; the captured imprint already contains its initial angle. */
      const float polar = std::atan2(radial.y,radial.x);
      state.angle = state.rotation_base+polar-state.rotation_mouse_start;
    }
  }
  if (state.scaling) {
    state.footprint_scale = footprint_from_mouse(state.scale_base,
        math::length(event_mouse-state.target_mouse),state.scale_mouse_start);
  }
  if (state.strength_adjust) {
    state.imprint_strength = strength_from_mouse(state.strength_base,event_mouse.x,state.strength_mouse_start);
  }
  const std::optional<CursorGeometryInfo> hit = cursor_geometry_info_update(C, mouse, false);
  if (!hit) { flush_update_step(vc, ob, UpdateType::Position); return true; }
  const float3 normal = math::normalize(hit->normal);
  if (!finite(normal) || math::length_squared(normal) < .5f) { return fail(state,"Invalid destination normal"); }
  const float max_distance = std::max(state.radius*.25f, 1e-5f);
  const float support = state.support*std::max(1.0f,state.footprint_scale)+max_distance;
  IndexMaskMemory memory;
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const IndexMask leaves = bke::pbvh::all_leaf_nodes(pbvh,memory);
  const IndexMask candidates = IndexMask::from_predicate(leaves, memory,
      [&](int i) { return sphere_bounds(nodes[i].bounds(), hit->location, support); }, exec_mode::grain_size(64));
  /* Capture every intersecting grid node before changing its positions, including shared borders. */
  int64_t sample_count = 0;
  candidates.foreach_index([&](int i) { sample_count += int64_t(nodes[i].grids().size())*state.ccg->grid_area; });
  undo::push_nodes(depsgraph, ob, candidates, undo::Type::Position);
  auto &staged = state.pending;
  staged.clear();
  staged.reserve(size_t(sample_count));
  const int area = state.ccg->grid_area;
  candidates.foreach_index([&](int node) {
    for (const int grid : nodes[node].grids()) {
      for (int j=0;j<area;j++) {
        const int index = grid*area+j;
        if (!state.ccg->grid_hidden.is_empty() && state.ccg->grid_hidden[grid][j]) { continue; }
        const float3 p = state.ccg->positions[index];
        if (math::distance_squared(p,hit->location) > support*support ||
            math::dot(state.ccg->normals[index],normal) < .25f) { continue; }
        const float3 unrotated = rotate(rotate_axis(p-hit->location,normal,-state.angle), normal, state.normal);
        const float3 query = state.center+footprint_inverse(unrotated,state.normal,state.footprint_scale);
        BVHTreeNearest nearest{};
        nearest.index = -1; nearest.dist_sq = max_distance*max_distance;
        BLI_bvhtree_find_nearest(state.tree, query, &nearest, nearest_cb, &state);
        if (nearest.index < 0) { continue; }
        const Triangle &tri = state.triangles[nearest.index];
        float3 w;
        if (!weights(tri,float3(nearest.co),w)) { continue; }
        const float3 source_normal = math::normalize(w.x*tri.normal[0]+w.y*tri.normal[1]+w.z*tri.normal[2]);
        if (math::dot(source_normal,state.normal) < .25f) { continue; }
        /* Do not extend the closest nonzero edge into a plateau outside the captured stamp. */
        const float3 separation = query-float3(nearest.co);
        const float3 face_normal = math::normalize(math::cross(tri.co[1]-tri.co[0],tri.co[2]-tri.co[0]));
        const float3 tangent = separation-face_normal*math::dot(separation,face_normal);
        const float tangent_tolerance = std::max(state.radius*1e-4f,1e-6f);
        if (math::length_squared(tangent) > tangent_tolerance*tangent_tolerance) { continue; }
        const float3 delta = w.x*tri.delta[0]+w.y*tri.delta[1]+w.z*tri.delta[2];
        float3 moved = rotate_axis(rotate(delta,source_normal,normal),normal,state.angle)*state.imprint_strength;
        /* Source attenuation is already captured; apply the destination's mask as well. */
        if (!state.ccg->masks.is_empty()) { moved *= std::clamp(1.0f-state.ccg->masks[index],0.0f,1.0f); }
        if (!finite(moved)) { continue; }
        if (math::length_squared(moved) > 1e-20f) { staged.push_back({index,moved}); }
      }
    }
  });
  if (staged.empty() && state.imprint_strength != 0.0f) { flush_update_step(vc, ob, UpdateType::Position); return true; }
  auto &grids = state.write_grids;
  auto &translations = state.write_translations;
  grids.clear();
  translations.clear();
  grids.reserve(staged.size());
  translations.reserve(size_t(sample_count));
  int previous_grid = -1;
  for (const State::Pending &p : staged) {
    const int grid = p.index/area;
    if (grid != previous_grid) {
      grids.push_back(grid);
      translations.resize(translations.size()+area,float3(0));
      previous_grid = grid;
    }
    translations[translations.size()-area+p.index%area] = p.delta;
  }
  /* Use the established writer: it records exact dirty grids AND samples for layer Commit. */
  if (!grids.empty()) { apply_translations(Span<float3>(translations.data(),translations.size()),
                     Span<int>(grids.data(),grids.size()),*state.ccg); }
  pbvh.tag_positions_changed(candidates);
  pbvh.update_bounds(depsgraph, ob);
  flush_update_step(vc, ob, UpdateType::Position);
  ss.cache->location = hit->location;
  ss.cache->location_symm = hit->location;
  sd.paint.runtime->anchored_initial_mouse[0] = mouse.x;
  sd.paint.runtime->anchored_initial_mouse[1] = mouse.y;
  sd.paint.runtime->brush_rotation = state.initial_brush_rotation+state.angle;
  sd.paint.runtime->brush_rotation_sec = state.initial_mask_rotation+state.angle;
  sd.paint.runtime->anchored_size = sd.paint.runtime->pixel_radius*state.footprint_scale;
  state.target_mouse = mouse;
  state.valid = true;
  char status[320];
  if (state.strength_adjust) {
    std::snprintf(status,sizeof(status),"Draw Transport STRENGTH %.3fx | mouse left/right | Click/Enter: accept | Esc: cancel stroke",state.imprint_strength);
  }
  else if (state.scaling) {
    std::snprintf(status,sizeof(status),"Draw Transport SCALE %.3fx (depth unchanged) | S again: strength | Click/Enter: accept | Esc: cancel stroke",state.footprint_scale);
  }
  else if (state.rotating) {
    std::snprintf(status,sizeof(status),"Draw Transport ROTATE %.1f deg | Click/Enter/R: accept | Esc: cancel stroke",state.angle*57.295779513f);
  }
  else {
    std::snprintf(status,sizeof(status),"Draw Transport MOVE | scale %.3fx strength %.3fx | R: rotate | S: scale / SS: strength | Click/Enter: confirm | Esc: cancel",state.footprint_scale,state.imprint_strength);
  }
  ED_workspace_status_text(C,status);
  return true;
}
/* Prepared transforms preserve the V04 arithmetic while moving constant trig/frame work out of the sample loop. */
struct PreparedAxisRotation {
  float3 axis;
  float cosine, sine;
  PreparedAxisRotation(const float3 &axis_, const float angle)
      : axis(axis_), cosine(std::cos(angle)), sine(std::sin(angle)) {}
  float3 apply(const float3 &v) const
  {
    return cosine*v+sine*math::cross(axis,v)+(1.0f-cosine)*math::dot(axis,v)*axis;
  }
};
struct PreparedNormalRotation {
  float3 axis = float3(0);
  float cosine = 1.0f, sine = 0.0f;
  int mode = 0;
  PreparedNormalRotation(const float3 &from, const float3 &to)
  {
    cosine=std::clamp(math::dot(from,to),-1.0f,1.0f);
    if (cosine>0.999999f) { return; }
    if (cosine< -0.9999f) {
      mode=1;
      axis=math::cross(from,float3(1,0,0));
      if (math::length_squared(axis)<1e-8f) { axis=math::cross(from,float3(0,1,0)); }
      axis=math::normalize(axis);
      return;
    }
    mode=2;
    axis=math::normalize(math::cross(from,to));
    sine=std::sqrt(std::max(0.0f,1.0f-cosine*cosine));
  }
  float3 apply(const float3 &v) const
  {
    if (!mode) { return v; }
    if (mode==1) { return 2.0f*math::dot(axis,v)*axis-v; }
    return cosine*v+sine*math::cross(axis,v)+(1.0f-cosine)*math::dot(axis,v)*axis;
  }
};
/* Read-only kernel: each worker owns one output slot and its local BVH query. */
inline float3 map_reference_sample(const State &state, const ReferenceSample &sample,
                                   const float3 &location, const float3 &normal,
                                   const float support, const float max_distance,
                                   const PreparedAxisRotation &inverse_angle,
                                   const PreparedNormalRotation &inverse_normal,
                                   const PreparedAxisRotation &output_angle,
                                   int *seed = nullptr, const int neighbor_seed = -1)
{
  if (sample.hidden || math::distance_squared(sample.position,location)>support*support ||
      math::dot(sample.normal,normal)<.25f) { return float3(0); }
  const float3 unrotated=inverse_normal.apply(inverse_angle.apply(sample.position-location));
  const float3 query=state.center+footprint_inverse(unrotated,state.normal,state.footprint_scale);
  BVHTreeNearest nearest{};
  nearest.index=-1; nearest.dist_sq=max_distance*max_distance;
  /* Coherent triangles give a valid upper distance bound, never a replacement
   * for the global nearest search. Do not seed the result index: ties still go
   * through the normal tree traversal. Inflate the bound to include its owner. */
  const float original_limit=nearest.dist_sq;
  if (seed) {
    BVHTreeNearest hint{};
    hint.index=-1;hint.dist_sq=original_limit;
    for (const int candidate : {*seed,neighbor_seed}) {
      if (candidate>=0 && size_t(candidate)<state.triangles.size()) {
        nearest_cb(const_cast<State *>(&state),candidate,query,&hint);
      }
    }
    if (hint.index>=0) { nearest.dist_sq=std::nextafter(hint.dist_sq,original_limit); }
  }
  BLI_bvhtree_find_nearest(state.tree,query,&nearest,nearest_cb,const_cast<State *>(&state));
  if (nearest.index<0 && nearest.dist_sq<original_limit) {
    /* Defensive fallback for floating-point boundary rejection. */
    nearest.dist_sq=original_limit;
    BLI_bvhtree_find_nearest(state.tree,query,&nearest,nearest_cb,const_cast<State *>(&state));
  }
  if (seed) { *seed=nearest.index; }
  if (nearest.index<0) { return float3(0); }
  const Triangle &tri=state.triangles[nearest.index];
  float3 w;
  if (!weights(tri,float3(nearest.co),w)) { return float3(0); }
  const float3 source_normal=math::normalize(w.x*tri.normal[0]+w.y*tri.normal[1]+w.z*tri.normal[2]);
  if (math::dot(source_normal,state.normal)<.25f) { return float3(0); }
  const float3 separation=query-float3(nearest.co);
  const float3 face_normal=math::normalize(math::cross(tri.co[1]-tri.co[0],tri.co[2]-tri.co[0]));
  const float3 tangent=separation-face_normal*math::dot(separation,face_normal);
  const float tolerance=std::max(state.radius*1e-4f,1e-6f);
  if (math::length_squared(tangent)>tolerance*tolerance) { return float3(0); }
  const float3 delta=w.x*tri.delta[0]+w.y*tri.delta[1]+w.z*tri.delta[2];
  return output_angle.apply(rotate(delta,source_normal,normal));
}
inline float3 weighted_delta(const float3 &mapped, const float strength, const float mask_factor)
{
  /* Keep the original order: imprint Strength first, destination mask second. */
  return (mapped*strength)*mask_factor;
}
inline bool effective_delta(const float3 &delta)
{
  return finite(delta) && math::length_squared(delta)>1e-20f;
}
inline IndexMask index_mask(const std::vector<int> &indices, IndexMaskMemory &memory)
{
  return IndexMask::from_indices(Span<int>(indices.data(),indices.size()),memory);
}
inline int64_t restore_preview(State &state, bke::pbvh::Tree &pbvh)
{
  const int64_t count=state.preview_before.size();
  /* Exact assignments avoid subtraction drift over repeated moves/SS adjustments. */
  for (const SavedPosition &saved:state.preview_before) {
    state.ccg->positions[saved.index]=saved.position;
  }
  if (!state.preview_nodes.empty()) {
    IndexMaskMemory memory;
    pbvh.tag_positions_changed(index_mask(state.preview_nodes,memory));
  }
  state.preview_before.clear();
  state.preview_nodes.clear();
  return count;
}
inline void profile_frame(State &state, const std::array<double,5> &times,
                            const int64_t restored, const bool reused_region,
                            const bool reused_mapping)
{
  if (!state.profile) { return; }
  state.profile_frames++;
  for (int i=0;i<5;i++) { state.profile_total[i]+=times[i]; }
  if (state.profile_frames<=8 || state.profile_frames%15==0) {
    std::printf("[DRAW_TRANSPORT_V06_FRAME] frame=%lld mode=%s restored=%lld samples=%zu reuse_region=%d reuse_mapping=%d parallel=%d restore_ms=%.3f surface_ms=%.3f map_ms=%.3f write_ms=%.3f update_ms=%.3f total_ms=%.3f\n",
        static_cast<long long>(state.profile_frames),state.strength_adjust?"SS":state.scaling?"S":state.rotating?"R":"MOVE",
        static_cast<long long>(restored),state.region_samples.size(),int(reused_region),int(reused_mapping),int(state.parallel),
        times[0],times[1],times[2],times[3],times[4],times[0]+times[1]+times[2]+times[3]+times[4]);
    std::fflush(stdout);
  }
}
inline void update_status(State &state,bContext *C)
{
  char status[320];
  if (state.strength_adjust) {
    std::snprintf(status,sizeof(status),"Draw Transport STRENGTH %.3fx | mouse left/right | Click/Enter: accept | Esc: cancel stroke",state.imprint_strength);
  }
  else if (state.scaling) {
    std::snprintf(status,sizeof(status),"Draw Transport SCALE %.3fx (depth unchanged) | S again: strength | Click/Enter: accept | Esc: cancel stroke",state.footprint_scale);
  }
  else if (state.rotating) {
    std::snprintf(status,sizeof(status),"Draw Transport ROTATE %.1f deg | Click/Enter/R: accept | Esc: cancel stroke",state.angle*57.295779513f);
  }
  else {
    std::snprintf(status,sizeof(status),"Draw Transport MOVE | scale %.3fx strength %.3fx | R: rotate | S: scale / SS: strength | Click/Enter: confirm | Esc: cancel",state.footprint_scale,state.imprint_strength);
  }
  ED_workspace_status_text(C,status);
}
inline bool move_optimized(State &state,bContext *C,ViewContext &vc,const wmEvent &event)
{
  Object &ob=*CTX_data_active_object(C);
  SculptSession &ss=*ob.runtime->sculpt_session;
  Sculpt &sd=*CTX_data_tool_settings(C)->sculpt;
  Depsgraph &depsgraph=*CTX_data_depsgraph_pointer(C);
  if (ss.subdiv_ccg!=state.ccg || !ss.cache || state.ccg->positions.size()!=state.positions_num) {
    return fail(state,"Multires surface changed during positioning");
  }
  std::array<double,5> times={};
  double phase=BLI_time_now_seconds();
  bke::pbvh::Tree &pbvh=*bke::object::pbvh_get(ob);
  const int64_t restored=restore_preview(state,pbvh);
  times[0]=(BLI_time_now_seconds()-phase)*1000.0; phase=BLI_time_now_seconds();
  state.valid=false;
  const float2 event_mouse(float(event.mval[0]),float(event.mval[1]));
  const float2 mouse=state.adjusting()?state.target_mouse:event_mouse+state.mouse_offset;
  if (state.rotating) {
    const float2 radial=event_mouse-state.target_mouse;
    if (math::length_squared(radial)>=4.0f) {
      state.angle=state.rotation_base+std::atan2(radial.y,radial.x)-state.rotation_mouse_start;
    }
  }
  if (state.scaling) {
    state.footprint_scale=footprint_from_mouse(state.scale_base,math::length(event_mouse-state.target_mouse),state.scale_mouse_start);
  }
  if (state.strength_adjust) {
    state.imprint_strength=strength_from_mouse(state.strength_base,event_mouse.x,state.strength_mouse_start);
  }
  const bool same_target=state.target_cached && mouse.x==state.cached_mouse.x && mouse.y==state.cached_mouse.y;
  bool baseline_updated=false;
  if (!same_target) {
    /* Current geometry is pristine after sparse restore, before raycast and reference capture. */
    pbvh.update_bounds(depsgraph,ob);
    bke::pbvh::update_normals(depsgraph,ob,pbvh);
    baseline_updated=true;
    const std::optional<CursorGeometryInfo> hit=cursor_geometry_info_update(C,mouse,false);
    state.target_cached=state.region_cached=state.mapping_cached=false;
    if (!hit) {
      flush_update_step(vc,ob,UpdateType::Position);
      times[1]=(BLI_time_now_seconds()-phase)*1000.0;
      profile_frame(state,times,restored,false,false);
      return true;
    }
    state.cached_location=hit->location;
    state.cached_normal=math::normalize(hit->normal);
    if (!finite(state.cached_normal) || math::length_squared(state.cached_normal)<.5f) {
      return fail(state,"Invalid destination normal");
    }
    state.cached_mouse=mouse;
    state.target_cached=true;
  }
  const float3 location=state.cached_location,normal=state.cached_normal;
  const float max_distance=std::max(state.radius*.25f,1e-5f);
  const float support=state.support*std::max(1.0f,state.footprint_scale)+max_distance;
  const bool reused_region=state.region_cached && same_target && support<=state.region_support;
  const int area=state.ccg->grid_area;
  if (!reused_region) {
    if (!baseline_updated) {
      pbvh.update_bounds(depsgraph,ob);
      bke::pbvh::update_normals(depsgraph,ob,pbvh);
    }
    IndexMaskMemory memory;
    /* Hierarchical PBVH traversal replaces a scan of every leaf node. */
    const IndexMask candidates=bke::pbvh::search_nodes(pbvh,memory,[&](const bke::pbvh::Node &node) {
      return sphere_bounds(node.bounds(),location,support);
    });
    undo::push_nodes(depsgraph,ob,candidates,undo::Type::Position);
    state.region_nodes.clear();state.region_grids.clear();
    const Span<bke::pbvh::GridsNode> nodes=pbvh.nodes<bke::pbvh::GridsNode>();
    candidates.foreach_index([&](int i) {
      state.region_nodes.push_back(i);
      for (const int grid:nodes[i].grids()) { state.region_grids.push_back(grid); }
    });
    state.region_samples.resize(state.region_grids.size()*size_t(area));
    const auto capture_region=[&](const IndexRange range) {
      for (const int64_t offset:range) {
        const int grid=state.region_grids[offset/area],local=int(offset%area),index=grid*area+local;
        ReferenceSample sample;
        sample.index=index;sample.position=state.ccg->positions[index];sample.normal=state.ccg->normals[index];
        sample.mask_factor=state.ccg->masks.is_empty()?1.0f:std::clamp(1.0f-state.ccg->masks[index],0.0f,1.0f);
        sample.hidden=!state.ccg->grid_hidden.is_empty() && state.ccg->grid_hidden[grid][local];
        state.region_samples[offset]=sample;
      }
    };
    if (state.parallel) { threading::parallel_for(IndexRange(state.region_samples.size()),4096,capture_region); }
    else { capture_region(IndexRange(state.region_samples.size())); }
    state.region_support=support;
    state.region_cached=true;state.mapping_cached=false;
    state.nearest_seeds.assign(state.region_samples.size(),-1);
  }
  times[1]=(BLI_time_now_seconds()-phase)*1000.0;phase=BLI_time_now_seconds();
  const bool reused_mapping=state.mapping_cached && reused_region &&
      state.angle==state.mapped_angle && state.footprint_scale==state.mapped_scale;
  if (!reused_mapping) {
    state.mapped_deltas.resize(state.region_samples.size());
    const PreparedAxisRotation inverse_angle(normal,-state.angle),output_angle(normal,state.angle);
    const PreparedNormalRotation inverse_normal(normal,state.normal);
    const auto map_range=[&](const IndexRange range) {
      int neighbor_seed=-1;
      for (const int64_t i:range) {
        state.mapped_deltas[i]=map_reference_sample(state,state.region_samples[i],location,normal,
            support,max_distance,inverse_angle,inverse_normal,output_angle,
            state.coherent ? &state.nearest_seeds[i] : nullptr,neighbor_seed);
        neighbor_seed=state.nearest_seeds[i];
      }
    };
    if (state.parallel) { threading::parallel_for(IndexRange(state.region_samples.size()),1024,map_range); }
    else { map_range(IndexRange(state.region_samples.size())); }
    state.mapped_angle=state.angle;state.mapped_scale=state.footprint_scale;state.mapping_cached=true;
  }
  times[2]=(BLI_time_now_seconds()-phase)*1000.0;phase=BLI_time_now_seconds();
  auto &grids=state.write_grids;auto &translations=state.write_translations;
  grids.clear();translations.clear();
  state.preview_before.reserve(state.region_samples.size());
  grids.reserve(state.region_grids.size());translations.reserve(state.region_samples.size());
  /* Build all backups and write buffers before any mutation; failures use native stroke cancellation. */
  for (size_t g=0;g<state.region_grids.size();g++) {
    const size_t start=g*area;
    bool grid_written=false;
    for (int j=0;j<area;j++) {
      const size_t i=start+j;
      const ReferenceSample &sample=state.region_samples[i];
      const float3 delta=weighted_delta(state.mapped_deltas[i],state.imprint_strength,sample.mask_factor);
      if (!effective_delta(delta)) { continue; }
      if (!grid_written) {
        grids.push_back(state.region_grids[g]);
        translations.resize(translations.size()+area,float3(0));
        grid_written=true;
      }
      translations[translations.size()-area+j]=delta;
      state.preview_before.push_back({sample.index,sample.position});
    }
  }
  if (!grids.empty()) {
    state.preview_nodes=state.region_nodes;
    const auto write_range=[&](const IndexRange range) {
      apply_translations(Span<float3>(translations.data()+range.start()*area,range.size()*area),
                         Span<int>(grids.data()+range.start(),range.size()),*state.ccg);
    };
    if (state.parallel) { threading::parallel_for(IndexRange(grids.size()),std::max(1,4096/area),write_range); }
    else { write_range(IndexRange(grids.size())); }
  }
  times[3]=(BLI_time_now_seconds()-phase)*1000.0;phase=BLI_time_now_seconds();
  if (!state.preview_nodes.empty()) {
    IndexMaskMemory memory;pbvh.tag_positions_changed(index_mask(state.preview_nodes,memory));
  }
  pbvh.update_bounds(depsgraph,ob);
  flush_update_step(vc,ob,UpdateType::Position);
  state.valid=!grids.empty() || state.imprint_strength==0.0f;
  if (state.valid) {
    ss.cache->location=location;ss.cache->location_symm=location;
    sd.paint.runtime->anchored_initial_mouse[0]=mouse.x;sd.paint.runtime->anchored_initial_mouse[1]=mouse.y;
    sd.paint.runtime->brush_rotation=state.initial_brush_rotation+state.angle;
    sd.paint.runtime->brush_rotation_sec=state.initial_mask_rotation+state.angle;
    sd.paint.runtime->anchored_size=sd.paint.runtime->pixel_radius*state.footprint_scale;
    state.target_mouse=mouse;update_status(state,C);
  }
  times[4]=(BLI_time_now_seconds()-phase)*1000.0;
  profile_frame(state,times,restored,reused_region,reused_mapping);
  return true;
}
inline bool move(State &state,bContext *C,ViewContext &vc,const wmEvent &event)
{
  if (state.reference_path) {
    const double start=BLI_time_now_seconds();
    const bool result=move_reference(state,C,vc,event);
    if (state.profile) {
      std::printf("[DRAW_TRANSPORT_V06_REFERENCE] mode=%s total_ms=%.3f\n",
                  state.strength_adjust?"SS":state.scaling?"S":state.rotating?"R":"MOVE",
                  (BLI_time_now_seconds()-start)*1000.0);
    }
    return result;
  }
  return move_optimized(state,C,vc,event);
}
}  // namespace draw_transport_v01
