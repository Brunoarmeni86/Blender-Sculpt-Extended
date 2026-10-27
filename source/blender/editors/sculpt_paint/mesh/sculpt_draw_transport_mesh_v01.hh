/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Mesh backend: included within draw_transport_v01 after the shared mapping helpers. */
inline void mesh_source_triangle(State &state, const Triangle &tri)
{
  for (const float3 &co : tri.co) {
    const float depth = math::dot(co-state.center, state.normal);
    state.source_depth_min = std::min(state.source_depth_min, depth);
    state.source_depth_max = std::max(state.source_depth_max, depth);
    if (!state.source_bounds_valid) { state.source_min=state.source_max=co;state.source_bounds_valid=true; }
    for (int axis=0;axis<3;axis++) {
      state.source_min[axis]=std::min(state.source_min[axis],co[axis]);
      state.source_max[axis]=std::max(state.source_max[axis],co[axis]);
    }
  }
  state.triangles.push_back(tri);
}
inline bool capture_mesh(State &state, Object &ob, const Depsgraph &depsgraph)
{
  SculptSession &ss=*ob.runtime->sculpt_session;
  if (!ss.cache || ss.cache->radius<=1e-8f) { return fail(state,"No active mesh imprint or zero radius"); }
  const Mesh &mesh=*id_cast<Mesh *>(ob.data);
  const auto &pbvh=*bke::object::pbvh_get(ob);
  if (pbvh.type()!=bke::pbvh::Type::Mesh) { return fail(state,"Mesh transport requires stable mesh topology"); }
  state.mesh_backend=true;state.mesh_data=&mesh;state.positions_num=mesh.verts_num;
  state.center=ss.cache->location;state.normal=math::normalize(ss.cache->sculpt_normal);
  state.radius=ss.cache->radius;
  state.initial_brush_rotation=ss.cache->paint->runtime->brush_rotation;
  state.initial_mask_rotation=ss.cache->paint->runtime->brush_rotation_sec;
  if (!finite(state.normal) || math::length_squared(state.normal)<.5f) { return fail(state,"Invalid source normal"); }
  const Span<float3> positions=bke::pbvh::vert_positions_eval(depsgraph,ob);
  const Span<float3> normals=bke::pbvh::vert_normals_eval(depsgraph,ob);
  Map<int,ReferenceSample> originals;
  Set<int> faces;
  const auto vert_faces=mesh.vert_to_face_map();
  const auto nodes=pbvh.nodes<bke::pbvh::MeshNode>();
  for (const auto &node:nodes) {
    const auto original=orig_position_data_lookup_mesh(ob,node);
    if (!original) { continue; }
    const Span<int> verts=node.verts();
    if (original->positions.size()!=verts.size() || original->normals.size()!=verts.size()) {
      return fail(state,"Undo/source mesh layout differs");
    }
    state.capture_samples+=verts.size();
    state.preview_nodes.push_back(int(&node-nodes.data()));
    for (const int j:verts.index_range()) {
      const int index=verts[j];
      ReferenceSample sample;sample.index=index;sample.position=original->positions[j];sample.normal=original->normals[j];
      originals.add(index,sample);
      const float3 delta=positions[index]-sample.position;
      if (!finite(delta)) { return fail(state,"Nonfinite mesh imprint"); }
      if (math::length_squared(delta)>0.0f) {
        state.source_deltas.push_back({index,delta});state.preview_before.push_back({index,sample.position});
      }
      for (int face:vert_faces[index]) { faces.add(face); }
    }
  }
  if (state.source_deltas.empty()) { return fail(state,"No displacement in mesh imprint"); }
  const auto corner_verts=mesh.corner_verts();const auto tris=mesh.corner_tris();
  for (const int face:faces) {
    for (const int ti:bke::mesh::face_triangles_range(mesh.faces(),face)) {
      Triangle tri;bool changed=false;
      for (int j=0;j<3;j++) {
        const int vertex=corner_verts[tris[ti][j]];
        const auto *original=originals.lookup_ptr(vertex);
        tri.co[j]=original?original->position:positions[vertex];
        tri.normal[j]=original?original->normal:normals[vertex];
        tri.delta[j]=positions[vertex]-tri.co[j];
        changed|=math::length_squared(tri.delta[j])>1e-20f;
      }
      if (math::length_squared(math::cross(tri.co[1]-tri.co[0],tri.co[2]-tri.co[0]))<1e-24f) { continue; }
      if (changed) { for (const float3 &co:tri.co) { state.support=std::max(state.support,math::distance(co,state.center)); } }
      mesh_source_triangle(state,tri);
    }
  }
  if (state.triangles.empty() || state.triangles.size()>size_t(INT_MAX)) { return fail(state,"Invalid mesh imprint triangle count"); }
  std::sort(state.source_deltas.begin(),state.source_deltas.end(),[](const State::Pending &a,const State::Pending &b){return a.index<b.index;});
  state.tree=BLI_bvhtree_new(int(state.triangles.size()),0.0f,4,6);
  if (!state.tree) { return fail(state,"Cannot allocate source mesh BVH"); }
  for (int i=0;i<int(state.triangles.size());i++) { BLI_bvhtree_insert(state.tree,i,state.triangles[i].co[0],3); }
  BLI_bvhtree_balance(state.tree);state.placing=state.valid=true;
  std::printf("[DRAW_TRANSPORT_MESH] captured samples=%lld triangles=%zu\n",static_cast<long long>(state.capture_samples),state.triangles.size());
  return true;
}
inline bool move_mesh(State &state,bContext *C,ViewContext &vc,const wmEvent &event)
{
  Object &ob=*CTX_data_active_object(C);SculptSession &ss=*ob.runtime->sculpt_session;
  Depsgraph &depsgraph=*CTX_data_depsgraph_pointer(C);Sculpt &sd=*CTX_data_tool_settings(C)->sculpt;
  Mesh &mesh=*id_cast<Mesh *>(ob.data);auto &pbvh=*bke::object::pbvh_get(ob);
  if (!ss.cache || ss.subdiv_ccg || &mesh!=state.mesh_data || mesh.verts_num!=state.positions_num ||
      pbvh.type()!=bke::pbvh::Type::Mesh) { return fail(state,"Mesh topology changed during transport"); }
  /* Native writer maintains active L0 deltas, ordinary mesh positions and Undo independently. */
  if (!state.preview_before.empty()) {
    const PositionDeformData data(depsgraph,ob);
    Vector<int> vertices;Vector<float3> translations;
    vertices.reserve(state.preview_before.size());translations.reserve(state.preview_before.size());
    for (const auto &sample:state.preview_before) {vertices.append(sample.index);translations.append(sample.position-data.eval[sample.index]);}
    data.deform(translations,vertices);state.preview_before.clear();
    IndexMaskMemory memory;pbvh.tag_positions_changed(index_mask(state.preview_nodes,memory));
  }
  pbvh.update_bounds(depsgraph,ob);bke::pbvh::update_normals(depsgraph,ob,pbvh);
  state.valid=false;
  const float2 event_mouse(float(event.mval[0]),float(event.mval[1]));
  const float2 mouse=state.adjusting()?state.target_mouse:event_mouse+state.mouse_offset;
  if (state.rotating) {const float2 radial=event_mouse-state.target_mouse;if (math::length_squared(radial)>=4) {state.angle=state.rotation_base+std::atan2(radial.y,radial.x)-state.rotation_mouse_start;}}
  if (state.scaling) {state.footprint_scale=footprint_from_mouse(state.scale_base,math::length(event_mouse-state.target_mouse),state.scale_mouse_start);}
  if (state.strength_adjust) {state.imprint_strength=strength_from_mouse(state.strength_base,event_mouse.x,state.strength_mouse_start);}
  const auto hit=cursor_geometry_info_update(C,mouse,false);
  if (!hit) {flush_update_step(vc,ob,UpdateType::Position);return true;}
  float3 normal=math::normalize(hit->normal);
  if (!finite(normal) || math::length_squared(normal)<.5f) {return fail(state,"Invalid target normal");}
  const Brush *brush=BKE_paint_brush_for_read(&sd.paint);
  if (brush && brush->normal_radius_factor>0) {
    const float nr=state.radius*brush->normal_radius_factor;
    IndexMaskMemory memory;const auto nodes=bke::pbvh::search_nodes(pbvh,memory,[&](const bke::pbvh::Node &node){return sphere_bounds(node.bounds(),hit->location,nr);});
    const float3 old=ss.cache->location,old_symm=ss.cache->location_symm;
    ss.cache->location=ss.cache->location_symm=hit->location;
    const auto averaged=calc_area_normal(depsgraph,*brush,ob,nodes);
    ss.cache->location=old;ss.cache->location_symm=old_symm;
    if (averaged && finite(*averaged) && math::length_squared(*averaged)>.5f && math::dot(*averaged,normal)>.25f) {normal=math::normalize(*averaged);}
  }
  const float epsilon=std::max(state.radius*1e-5f,1e-7f);
  const bool original_pose=state.footprint_scale==1 && state.angle==0 && math::distance_squared(hit->location,state.center)<=epsilon*epsilon;
  if (original_pose) {normal=state.normal;}
  const float max_distance=std::max(state.radius*.25f,1e-5f);
  const float support=state.support*std::max(1.0f,state.footprint_scale)+max_distance;
  IndexMaskMemory memory;const auto candidates=bke::pbvh::search_nodes(pbvh,memory,[&](const bke::pbvh::Node &node){return sphere_bounds(node.bounds(),hit->location,support);});
  undo::push_nodes(depsgraph,ob,candidates,undo::Type::Position);
  const PositionDeformData data(depsgraph,ob);
  const auto normals=bke::pbvh::vert_normals_eval(depsgraph,ob);
  const MeshAttributeData attributes(mesh);
  const auto nodes=pbvh.nodes<bke::pbvh::MeshNode>();
  Set<int> target_faces;const auto vert_faces=mesh.vert_to_face_map();
  state.region_samples.clear();state.preview_nodes.clear();
  candidates.foreach_index([&](int n){
    state.preview_nodes.push_back(n);
    for (int vertex:nodes[n].verts()) {
      ReferenceSample sample;sample.index=vertex;sample.position=data.eval[vertex];sample.normal=normals[vertex];
      sample.hidden=!attributes.hide_vert.is_empty() && attributes.hide_vert[vertex];
      sample.mask_factor=attributes.mask.is_empty()?1.0f:std::clamp(1.0f-attributes.mask[vertex],0.0f,1.0f);
      state.region_samples.push_back(sample);
      for (int face:vert_faces[vertex]) {target_faces.add(face);}
    }
  });
  state.mesh_surface.clear();const auto cv=mesh.corner_verts();const auto tris=mesh.corner_tris();
  for (int face:target_faces) {for (int ti:bke::mesh::face_triangles_range(mesh.faces(),face)) {
    state.mesh_surface.push_back({data.eval[cv[tris[ti].x]],data.eval[cv[tris[ti].y]],data.eval[cv[tris[ti].z]]});
  }}
  if (state.surface_tree) {BLI_bvhtree_free(state.surface_tree);state.surface_tree=nullptr;}
  if (!state.mesh_surface.empty()) {
    state.surface_tree=BLI_bvhtree_new(int(state.mesh_surface.size()),0.0f,4,6);
    if (!state.surface_tree) {return fail(state,"Cannot allocate target mesh BVH");}
    for (int i=0;i<int(state.mesh_surface.size());i++) {BLI_bvhtree_insert(state.surface_tree,i,state.mesh_surface[i][0],3);}
    BLI_bvhtree_balance(state.surface_tree);
  }
  Vector<int> vertices;Vector<float3> translations;
  const PreparedAxisRotation inverse_angle(normal,-state.angle),output_angle(normal,state.angle);
  const PreparedNormalRotation inverse_normal(normal,state.normal);
  for (const auto &sample:state.region_samples) {
    if (sample.hidden) {continue;}
    float3 delta(0);
    if (original_pose) {
      const auto it=std::lower_bound(state.source_deltas.begin(),state.source_deltas.end(),sample.index,[](const State::Pending &v,int key){return v.index<key;});
      if (it!=state.source_deltas.end() && it->index==sample.index) {delta=it->delta*state.imprint_strength;}
    }
    else {
      delta=weighted_delta(map_reference_sample(state,sample,hit->location,normal,support,max_distance,inverse_angle,inverse_normal,output_angle),state.imprint_strength,sample.mask_factor);
      if (state.surface_tree && effective_delta(delta)) {
        const float eps=std::max(state.radius*1e-4f,1e-6f);BVHTreeRayHit occluder{};occluder.index=-1;occluder.dist=2*support;
        BLI_bvhtree_ray_cast(state.surface_tree,sample.position+normal*eps,normal,0,&occluder,surface_raycast_cb,&state);
        if (occluder.index>=0) {delta=float3(0);}
      }
    }
    if (!finite(delta)) {return fail(state,"Nonfinite transported mesh delta");}
    if (!effective_delta(delta)) {continue;}
    vertices.append(sample.index);translations.append(delta);state.preview_before.push_back({sample.index,sample.position});
  }
  if (!vertices.is_empty()) {data.deform(translations,vertices);pbvh.tag_positions_changed(candidates);}
  pbvh.update_bounds(depsgraph,ob);flush_update_step(vc,ob,UpdateType::Position);
  state.valid=!vertices.is_empty() || state.imprint_strength==0;
  if (state.valid) {
    ss.cache->location=ss.cache->location_symm=hit->location;
    sd.paint.runtime->anchored_initial_mouse[0]=mouse.x;sd.paint.runtime->anchored_initial_mouse[1]=mouse.y;
    sd.paint.runtime->brush_rotation=state.initial_brush_rotation+state.angle;
    sd.paint.runtime->brush_rotation_sec=state.initial_mask_rotation+state.angle;
    sd.paint.runtime->anchored_size=sd.paint.runtime->pixel_radius*state.footprint_scale;
    state.target_mouse=mouse;update_status(state,C);
  }
  return true;
}
