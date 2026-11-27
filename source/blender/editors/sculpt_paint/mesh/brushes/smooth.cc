/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include "DNA_brush_types.h"
#include "smooth_preserve_shape.hh"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_boundary.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"
#include "editors/sculpt_paint/mesh/sculpt_smooth.hh"

#include "bmesh.hh"
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
#  include "physics_brush_v15.hh"
#endif
#include "editors/sculpt_paint/mesh/sculpt_layer_delta_smooth_v01.hh"

#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
#  include "brush_solver_backend.hh"
#  include "d12_smooth_multiscale.hh"
#  include "d13_multiscale_flatten.hh"
#  include "brush_contact.hh"
#  include "BKE_multires_sculpt_layers.hh"
#endif

namespace blender::ed::sculpt_paint::brushes {

inline namespace smooth_cc {

static Vector<float> iteration_strengths(const float strength)
{
  constexpr int max_iterations = 4;

  BLI_assert_msg(strength >= 0.0f,
                 "The smooth brush expects a non-negative strength to behave properly");
  const float clamped_strength = std::min(strength, 1.0f);

  const int count = int(clamped_strength * max_iterations);
  const float last = max_iterations * (clamped_strength - float(count) / max_iterations);
  Vector<float> result;
  result.append_n_times(1.0f, count);
  result.append(last);
  return result;
}

struct LocalData {
  Vector<float3> positions;
  Vector<float3> normals;
  Vector<float> factors;
  Vector<float> distances;
  Vector<int> neighbor_offsets;
  Vector<int> neighbor_data;
  Vector<float3> new_positions;
  Vector<float3> translations;
};

BLI_NOINLINE static void apply_positions_faces(const Sculpt &sd,
                                               const Brush &brush,
                                               const Span<float3> vert_normals,
                                               const bke::pbvh::MeshNode &node,
                                               Object &object,
                                               LocalData &tls,
                                               const Span<float> factors,
                                               const Span<float3> new_positions,
                                               const PositionDeformData &position_data)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;

  const Span<int> verts = node.verts();

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_new_positions(new_positions, verts, position_data.eval, translations);
  /* SCULPT_SMOOTH_PRESERVE_SHAPE_V1 */
  if (brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE) {
    for (const int i : verts.index_range()) {
      smooth_preserve_shape_translation(translations[i], vert_normals[verts[i]]);
    }
  }
  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
/* Reuse the SAME native Smooth interior-neighbor averaging and per-iteration
 * strengths. XPBD changes how finished Smooth targets satisfy material bonds;
 * it does not replace Blender's Smooth brush with uniform Laplacian targets.
 * Active Sculpt Layer delta isolation is NOT supported in this first preview. */
static bool try_smooth_physics_v15(const Depsgraph &depsgraph,
                                  const Sculpt &sd,
                                  const Brush &brush,
                                  Object &object,
                                  const IndexMask &node_mask,
                                  const float brush_strength)
{
  if (!physics_v15::requested(object,brush)) return false;
  const SculptSession &ss=*object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh=*bke::object::pbvh_get(object);
  if (pbvh.type()!=bke::pbvh::Type::Mesh) {
    return physics_v15::freeze_if_mode_changed(object,pbvh,brush);
  }
  const auto &delta=layer_delta_smooth_v01::state();
  // Layer delta smoothing requires a separate Multires/SculptLayer writer.
  if (delta.object==&object && delta.mesh_l0) return false;
  Mesh &mesh=*id_cast<Mesh *>(object.data);
  const OffsetIndices faces=mesh.faces();
  const Span<int> corner_verts=mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map=mesh.vert_to_face_map();
  const MeshAttributeData attributes(mesh);
  const PositionDeformData pos(depsgraph,object);
  const Span<float3> normals=bke::pbvh::vert_normals_eval(depsgraph,object);
  MutableSpan<bke::pbvh::MeshNode> nodes=pbvh.nodes<bke::pbvh::MeshNode>();

  for (const float strength : iteration_strengths(brush_strength)) {
    LocalData tls;
    std::vector<sculpt_physics::NativeBrushTranslation> native;
    node_mask.foreach_index([&](const int ni) {
      const bke::pbvh::MeshNode &node=nodes[ni];
      const Span<int> verts=node.verts();
      calc_factors_common_mesh_indexed(
          depsgraph,brush,object,attributes,pos.eval,normals,node,
          tls.factors,tls.distances);
      scale_factors(tls.factors,strength);
      const GroupedSpan<int> neighbors=calc_vert_neighbors_interior(
          faces,corner_verts,vert_to_face_map,
          ss.boundary_info_cache->verts,
          ss.boundary_info_cache->edges,
          attributes.hide_poly,verts,tls.factors,
          tls.neighbor_offsets,tls.neighbor_data);
      tls.new_positions.resize(verts.size());
      smooth::neighbor_data_average_mesh_check_loose(
          pos.eval,verts,neighbors,tls.new_positions.as_mutable_span());
      tls.translations.resize(verts.size());
      translations_from_new_positions(
          tls.new_positions,verts,pos.eval,tls.translations);
      if (brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE) {
        for (const int j : verts.index_range()) {
          smooth_preserve_shape_translation(
              tls.translations[j],normals[verts[j]]);
        }
      }
      scale_translations(tls.translations,tls.factors);
      for (const int j : verts.index_range()) {
        const float3 v=tls.translations[j];
        native.push_back({std::size_t(verts[j]),
                          {double(v.x),double(v.y),double(v.z)}});
      }
    });
    if (!physics_v15::try_apply(
            sd,object,node_mask,pos,
            sculpt_physics::NativeSculptBrushKind::Smooth,native)) {
      return false; // V15 not activated; stock mesh smoothing takes over
    }
    // The native PBVH writer runs synchronously in try_apply; the next
    // iteration therefore reads the updated mesh, like the stock brush.
  }
  return true;
}
#endif

#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
static double smooth_local_contact_guard(const Mesh &mesh,const Span<float3> positions,
                                        std::unordered_map<std::size_t,float3> &changes)
{
  const auto map=mesh.vert_to_face_map();
  const auto faces=mesh.faces();
  const auto corners=mesh.corner_verts();
  const auto tris=mesh.corner_tris();
  std::vector<int> ids;
  for (const auto &entry:changes) {
    for (const int f:map[int(entry.first)]) ids.push_back(f);
  }
  std::sort(ids.begin(),ids.end());
  ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
  const auto cv=[](const float3 &v)->sculpt_physics::Vec3 {
    return {double(v.x),double(v.y),double(v.z)};
  };
  std::unordered_map<std::size_t,std::size_t> local;
  std::vector<std::size_t> global;
  std::vector<sculpt_physics::Vec3> p, motion;
  std::vector<std::array<std::size_t,3>> triangles;
  const auto add_vertex=[&](std::size_t v) {
    const auto found=local.find(v);
    if(found!=local.end()) return found->second;
    const auto i=p.size();
    local.emplace(v,i);
    global.push_back(v);
    p.push_back(cv(positions[int(v)]));
    const auto it=changes.find(v);
    motion.push_back(it==changes.end()?sculpt_physics::Vec3{}:cv(it->second));
    return i;
  };
  for (const int f:ids) {
    for (const int t:bke::mesh::face_triangles_range(faces,f)) {
      const int3 c=tris[t];
      triangles.push_back({add_vertex(std::size_t(corners[c[0]])),
                           add_vertex(std::size_t(corners[c[1]])),
                           add_vertex(std::size_t(corners[c[2]]))});
    }
  }
  const auto result=sculpt_physics::limit_local_contact(p,triangles,motion,true);
  for(std::size_t i=0;i<global.size();++i){
    auto it=changes.find(global[i]);
    if(it!=changes.end())it->second=float3(float(motion[i].x),float(motion[i].y),float(motion[i].z));
  }
  return result.final_scale;
}

static bool gpu_smooth_requested()
{
  const char *enabled=std::getenv("BLENDER_SCULPT_GPU_SMOOTH_A1");
  return enabled && enabled[0]=='1' && enabled[1]=='\0';
}
static bool flatten_d13_requested()
{
  const char *mode=std::getenv("BLENDER_SCULPT_GPU_SMOOTH_MODE");
  return gpu_smooth_requested() && mode &&
         (STREQ(mode,"flatten") ||
          (STREQ(mode,"multiscale") && std::getenv("BLENDER_SCULPT_D13_FLATTEN")));
}
static bool multiscale_smooth_requested()
{
  const char *mode=std::getenv("BLENDER_SCULPT_GPU_SMOOTH_MODE");
  return gpu_smooth_requested() && mode && STREQ(mode,"multiscale");
}

/* D12 Very Strong: current PBVH region only. Do not initialize a full mesh
 * particle world, copy full topology, or run the old two-ring solver first.
 * Only one connected, locally graph-like sheet is accepted. Boundary,
 * hidden and nonwritable vertices are never published as new goals. */
static bool try_smooth_multiscale_d12(const Depsgraph &depsgraph,
    const Sculpt &sd,const Brush &brush,Object &object,const IndexMask &node_mask,
    const float brush_strength)
{
  const bool flatten=flatten_d13_requested();
  if(!multiscale_smooth_requested()&&!flatten)return false;
  namespace ms=sculpt_physics::dedicated_d12::smooth_multiscale;
  namespace flat=sculpt_physics::dedicated_d13::flatten;
  flat::Params flatten_params;
  const char *version=std::getenv("BLENDER_SCULPT_D13_FLATTEN_V2");
  const bool flatten_v2=flatten && version && STREQ(version,"1");
  double plane_follow=0.,edge_start=.65;
  flat::PlaneState pending_plane;
  if(flatten){
    flatten_params.mode=flat::parse(std::getenv("BLENDER_SCULPT_D13_FLATTEN"));
    if(flatten_params.mode==flat::Mode::Invalid ||
       !flat::number(std::getenv("BLENDER_SCULPT_D13_FLATTEN_MIX"),flatten_params.plane_mix,0.,1.) ||
       !flat::number(std::getenv("BLENDER_SCULPT_D13_FLATTEN_OFFSET"),flatten_params.offset,-.25,.25))
      return false;
    if(flatten_v2){
      plane_follow=flatten_params.mode==flat::Mode::Adaptive?.35:0.;
      if(!flat::number(std::getenv("BLENDER_SCULPT_D13_FLATTEN_FOLLOW"),plane_follow,0.,1.) ||
         !flat::number(std::getenv("BLENDER_SCULPT_D13_FLATTEN_EDGE"),edge_start,0.,.95))
        return false;
    }
  }
  SculptSession &ss=*object.runtime->sculpt_session;
  StrokeCache &cache=*ss.cache;
  bke::pbvh::Tree &pbvh=*bke::object::pbvh_get(object);
  const auto &layer=layer_delta_smooth_v01::state();
  if(pbvh.type()!=bke::pbvh::Type::Mesh || ss.deform_modifiers_active || ss.shapekey_active ||
     bke::multires_layers::base_modifier(object)!=nullptr ||
     (layer.object==&object && layer.mesh_l0) ||
     cache.reference_symmetry || cache.reference_combined ||
     cache.tile_pass!=0 || cache.radial_symmetry_pass!=0 || cache.mirror_symmetry_pass!=0)
    return false;
  const Mesh &mesh=*id_cast<const Mesh *>(object.data);
  if(stroke_symmetry_xyz_get(object)!=PAINT_SYMM_NONE ||
     mesh.radial_symmetry[0]!=1 || mesh.radial_symmetry[1]!=1 || mesh.radial_symmetry[2]!=1 ||
     (sd.paint.symmetry_flags & (PAINT_TILE_X | PAINT_TILE_Y | PAINT_TILE_Z))!=0)
    return false;
  const PositionDeformData pos(depsgraph,object);
  if(pos.eval.size()!=mesh.verts_num)return false;
  const MeshAttributeData attrs(mesh);
  const Span<float3> normals=bke::pbvh::vert_normals_eval(depsgraph,object);
  const auto faces=mesh.faces();
  const auto corners=mesh.corner_verts();
  const auto v2f=mesh.vert_to_face_map();
  MutableSpan<bke::pbvh::MeshNode> nodes=pbvh.nodes<bke::pbvh::MeshNode>();
  struct WriteBatch {int node=0;Vector<int> ids;Vector<float3> motion;};
  std::vector<WriteBatch> batches;
  std::shared_ptr<sculpt_physics::StrokeSolverSession> session;
  const auto start=std::chrono::steady_clock::now();
  bool gpu=false;
  std::size_t active=0;
  double guard_scale=1.;
  try {
    std::vector<ms::Sample> samples;
    std::vector<int> global;
    std::vector<std::pair<int,int>> links;
    std::unordered_map<int,std::size_t> slot;
    const auto cv=[](const float3& p)->sculpt_physics::Vec3{
      return {double(p.x),double(p.y),double(p.z)};
    };
    LocalData tls;
    bool rejected=false;
    node_mask.foreach_index([&](const int ni){
      if(rejected)return;
      const auto &node=nodes[ni];
      const Span<int> verts=node.verts();
      tls.factors.resize(verts.size());tls.distances.resize(verts.size());
      calc_factors_common_mesh_indexed(depsgraph,brush,object,attrs,pos.eval,normals,node,
                                      tls.factors,tls.distances);
      scale_factors(tls.factors,std::clamp(brush_strength,0.f,1.f));
      const auto neighbors=calc_vert_neighbors_interior(faces,corners,v2f,
          ss.boundary_info_cache->verts,ss.boundary_info_cache->edges,attrs.hide_poly,verts,
          tls.factors,tls.neighbor_offsets,tls.neighbor_data);
      for(const int j:verts.index_range()){
        const int id=verts[j];
        const float factor=tls.factors[j];
        if(!(factor>0.f)||!std::isfinite(factor)||neighbors[j].is_empty())continue;
        if(boundary::vert_is_boundary(v2f,attrs.hide_poly,ss.boundary_info_cache->verts,id)){
          rejected=true;return;
        }
        if(samples.size()>=500000){rejected=true;return;}
        if(!slot.emplace(id,samples.size()).second)continue;
        double area=0.;
        for(const int neighbor:neighbors[j]){
          const auto q=cv(pos.eval[neighbor])-cv(pos.eval[id]);
          area+=sculpt_physics::dedicated_d12::norm2(q);
          links.emplace_back(id,neighbor);
        }
        area/=double(neighbors[j].size());
        samples.push_back({cv(pos.eval[id]),cv(normals[id]),std::clamp(double(factor),0.,1.),area});
        global.push_back(id);
      }
    });
    if(rejected)return false;
    if(samples.empty())return true;
    // Do not mix disconnected islands or opposite walls into one height map.
    std::vector<std::size_t> parent(samples.size());
    for(std::size_t i=0;i<parent.size();++i)parent[i]=i;
    const auto root=[&](std::size_t i){
      while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}
      return i;
    };
    for(const auto& edge:links){
      const auto a=slot.find(edge.first),b=slot.find(edge.second);
      if(a!=slot.end()&&b!=slot.end())parent[root(a->second)]=root(b->second);
    }
    const auto component=root(0);
    for(std::size_t i=1;i<parent.size();++i)if(root(i)!=component)return false;
    ms::Chart chart;
    if(!ms::prepare(samples,cv(cache.location_symm),double(cache.radius),chart))return false;
    active=samples.size();
    session=std::static_pointer_cast<sculpt_physics::StrokeSolverSession>(cache.physical_solver_session);
    if(!session || session->purpose!=sculpt_physics::StrokeSolverPurpose::Smooth ||
       session->vertex_count!=std::size_t(mesh.verts_num)){
      session=std::make_shared<sculpt_physics::StrokeSolverSession>();
      session->purpose=sculpt_physics::StrokeSolverPurpose::Smooth;
      session->vertex_count=std::size_t(mesh.verts_num);
      cache.physical_solver_session=session;
    }
    const bool need_filter=!flatten_v2 || flatten_params.mode==flat::Mode::Adaptive;
    std::vector<double> displacement;
    if(need_filter){
      if(!session->multiscale_attempted){
        session->multiscale_attempted=true;
        session->multiscale=ms::create_cuda();
      }
      gpu=session->multiscale && session->multiscale->evaluate(chart,displacement) &&
          ms::validate(chart,displacement);
      if(!gpu)displacement=ms::cpu(chart);
      if(!ms::validate(chart,displacement))return false;
    }
    if(flatten){
      std::vector<double> result;
      if(flatten_v2){
        pending_plane=session->flatten_plane;
        if(!flat::update_plane(chart,flatten_params.mode,plane_follow,pending_plane) ||
           !flat::evaluate_v2(chart,samples,displacement,flatten_params,pending_plane,edge_start,result))
          return false;
      }
      else if(!flat::evaluate(chart,displacement,flatten_params,result))return false;
      displacement=std::move(result);
    }
    // Stage exact native clipped motion before checking local triangles.
    std::unordered_map<std::size_t,float3> changes;
    changes.reserve(active);
    std::unordered_map<int,bool> staged;
    staged.reserve(active);
    node_mask.foreach_index([&](const int ni){
      WriteBatch batch;batch.node=ni;
      for(const int id:nodes[ni].verts()){
        const auto it=slot.find(id);
        if(it==slot.end()||!staged.emplace(id,true).second)continue;
        const auto d=chart.axis*(chart.radius*displacement[it->second]);
        float3 v{float(d.x),float(d.y),float(d.z)};
        if(brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE)
          smooth_preserve_shape_translation(v,normals[id]);
        if(math::length_squared(v)>0.f){batch.ids.append(id);batch.motion.append(v);}
      }
      if(batch.ids.is_empty())return;
      clip_and_lock_translations(sd,ss,pos.eval,batch.ids,batch.motion);
      for(const int i:batch.ids.index_range())
        changes.emplace(std::size_t(batch.ids[i]),batch.motion[i]);
      batches.push_back(std::move(batch));
    });
    if(changes.empty()){
      if(flatten_v2)session->flatten_plane=pending_plane;
      return true;
    }
    guard_scale=smooth_local_contact_guard(mesh,pos.eval,changes);
    for(auto& batch:batches)for(const int i:batch.ids.index_range()){
      batch.motion[i]=changes.at(std::size_t(batch.ids[i]));
      const auto v=batch.motion[i];
      if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))return false;
    }
  }catch(const std::exception& e){
    std::fprintf(stderr,"[D12 Multiscale] pre-write fallback: %s\n",e.what());
    return false;
  }
  // Existing Blender sculpt Undo transaction owns these positions. Do not
  // fall back to another algorithm after any geometry has been published.
  for(auto& batch:batches){
    pos.deform(batch.motion,batch.ids);
    bke::pbvh::update_node_bounds_mesh(pos.eval,nodes[batch.node]);
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  // A legacy GPU world (if a prior chart was rejected) is now stale.
  session->solver.reset();
  if(flatten_v2 && guard_scale>0.)session->flatten_plane=pending_plane;
  if(session->multiscale_dabs++%32==0 || std::getenv("BLENDER_SCULPT_SMOOTH_DIAG"))
    std::fprintf(stderr,"[D12 Multiscale %s] active=%zu guard=%.4f total_ms=%.3f%s\n",
      gpu?"Warp CUDA":"CPU multiscale",active,guard_scale,
      std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),
      brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE?" preserve-shape":"");
  if(flatten && std::getenv("BLENDER_SCULPT_SMOOTH_DIAG"))
    std::fprintf(stderr,"[D13 Flatten] version=%d mode=%s base=%s guard=%.4f follow=%.3f edge=%.3f\n",
                 flatten_v2?2:1,flat::name(flatten_params.mode),
                 flatten_v2 && flatten_params.mode!=flat::Mode::Adaptive?"direct CPU":gpu?"CUDA":"CPU",
                 guard_scale,plane_follow,edge_start);
  return true;
}

/* A1 hybrid validation path. Adjacency and positions stay resident in the
 * selected Warp backend for the stroke; Blender still computes brush factors
 * and the reference neighbor average. This deliberately validates lifetime,
 * active-region, Undo and dirty-node integration before moving factor and CSR
 * evaluation into a portable compute kernel. The normal Smooth path remains
 * the default and is called unchanged on every rejection. */
static bool try_gpu_smooth_a1(const Depsgraph &depsgraph,
                              const Sculpt &sd,
                              const Brush &brush,
                              Object &object,
                              const IndexMask &node_mask,
                              const float brush_strength)
{
  const char *enabled = std::getenv("BLENDER_SCULPT_GPU_SMOOTH_A1");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    return false;
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const auto &layer=layer_delta_smooth_v01::state();
  if (pbvh.type() != bke::pbvh::Type::Mesh || ss.deform_modifiers_active ||
      ss.shapekey_active || (layer.object==&object && layer.mesh_l0) ||
      bke::multires_layers::base_modifier(object)!=nullptr ||
      cache.tile_pass != 0 || cache.radial_symmetry_pass != 0)
  {
    return false;
  }

  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const PositionDeformData position_data(depsgraph, object);
  if (position_data.eval.size() != mesh.verts_num || mesh.verts_num == 0) {
    return false;
  }
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const MeshAttributeData attributes(mesh);
  const Span<float3> normals = bke::pbvh::vert_normals_eval(depsgraph, object);
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();

  try {
    auto session = std::static_pointer_cast<sculpt_physics::StrokeSolverSession>(
        cache.physical_solver_session);
    if (!session || session->purpose != sculpt_physics::StrokeSolverPurpose::Smooth ||
        session->vertex_count != std::size_t(mesh.verts_num) || !session->solver)
    {
      session = sculpt_physics::create_stroke_solver_session(
          sculpt_physics::StrokeSolverPurpose::Smooth,
          sculpt_physics::BackendKind::WarpCuda,
          std::size_t(mesh.verts_num));
      if (session->selected_backend != sculpt_physics::BackendKind::WarpCuda) {
        std::fprintf(stderr,"[GPU Smooth A1] CUDA backend unavailable; standard Smooth fallback\n");
        cache.physical_solver_session=session; // do not retry failed CUDA each dab
        return false;
      }
      sculpt_physics::BrushSolverBackend &solver = *session->solver;
      for (const int vertex : position_data.eval.index_range()) {
        const bool hidden = !attributes.hide_vert.is_empty() && attributes.hide_vert[vertex];
        const float mask = attributes.mask.is_empty() ? 0.0f : attributes.mask[vertex];
        const float3 p = position_data.eval[vertex];
        solver.add_particle({double(p.x), double(p.y), double(p.z)},
                            hidden || mask >= 0.999f ? 0.0 : 1.0 - double(mask));
      }
      for (const int2 edge : mesh.edges()) {
        if (edge[0] != edge[1]) {
          solver.add_bond(std::size_t(edge[0]), std::size_t(edge[1]), 0.0);
        }
      }
      /* Smooth must evolve from the previous dab, not preserve the rest shape
       * captured at stroke start. Grab and Clay keep the default material
       * stiffness because they depend on that shape retention. */
      solver.set_shape_stiffness_override(0.0);
      solver.begin_stroke();
      cache.physical_solver_session = session;
    }

    if(session->selected_backend!=sculpt_physics::BackendKind::WarpCuda)return false;
    sculpt_physics::BrushSolverBackend &solver = *session->solver;
    struct Target {
      int vertex;
      float factor;
      float3 position;
    };
    std::vector<Target> gathered;
    threading::EnumerableThreadSpecific<LocalData> all_tls;
    node_mask.foreach_index([&](const int node_i) {
      LocalData &tls = all_tls.local();
      const bke::pbvh::MeshNode &node = nodes[node_i];
      const Span<int> verts = node.verts();
      tls.factors.resize(verts.size());
      tls.distances.resize(verts.size());
      calc_factors_common_mesh_indexed(depsgraph,
                                       brush,
                                       object,
                                       attributes,
                                       position_data.eval,
                                       normals,
                                       node,
                                       tls.factors,
                                       tls.distances);
      scale_factors(tls.factors, std::clamp(brush_strength, 0.0f, 1.0f));
      const GroupedSpan<int> neighbors = calc_vert_neighbors_interior(faces,
                                                                      corner_verts,
                                                                      vert_to_face_map,
                                                                      ss.boundary_info_cache->verts,
                                                                      ss.boundary_info_cache->edges,
                                                                      attributes.hide_poly,
                                                                      verts,
                                                                      tls.factors,
                                                                      tls.neighbor_offsets,
                                                                      tls.neighbor_data);
      tls.new_positions.resize(verts.size());
      smooth::neighbor_data_average_mesh_check_loose(
          position_data.eval, verts, neighbors, tls.new_positions.as_mutable_span());
      for (const int i : verts.index_range()) {
        if (tls.factors[i] > 0.0f && std::isfinite(tls.factors[i])) {
          gathered.push_back({verts[i], tls.factors[i], tls.new_positions[i]});
        }
      }
    });
    if (gathered.empty()) {
      return true;
    }
    std::sort(gathered.begin(), gathered.end(), [](const Target &a, const Target &b) {
      return a.vertex < b.vertex;
    });
    std::vector<sculpt_physics::World::BrushTargetInput> targets;
    std::vector<std::size_t> active_region;
    std::vector<float> compact_weights;
    std::vector<sculpt_physics::Vec3> compact_normals;
    for (std::size_t i = 0; i < gathered.size();) {
      Target target = gathered[i++];
      while (i < gathered.size() && gathered[i].vertex == target.vertex) {
        if (gathered[i].factor > target.factor) {
          target = gathered[i];
        }
        i++;
      }
      const double weight = std::clamp(double(target.factor), 1.0e-4, 1.0);
      const double compliance = (1.0 / weight - 1.0) / 250000.0;
      targets.push_back({std::size_t(target.vertex),
                         {double(target.position.x),
                          double(target.position.y),
                          double(target.position.z)},
                         compliance});
      active_region.push_back(std::size_t(target.vertex));
      compact_weights.push_back(float(weight));
      const float3 normal = normals[target.vertex];
      compact_normals.push_back({double(normal.x), double(normal.y), double(normal.z)});
    }
    /* A2 uses a dedicated Laplacian kernel. The full resident CSR gives every
     * active vertex access to one-ring neighbours while only active vertices
     * are written: the surrounding collar is therefore read-only. Per-dab
     * traffic and readback are compact. Keep A1 as an atomic compatibility
     * fallback for an older PTX package. */
    sculpt_physics::SmoothSolverMode smooth_mode = sculpt_physics::SmoothSolverMode::Laplacian;
    bool physical_relax = false;
    if (const char *mode = std::getenv("BLENDER_SCULPT_GPU_SMOOTH_MODE")) {
      if (STREQ(mode, "taubin")) {
        smooth_mode = sculpt_physics::SmoothSolverMode::Taubin;
      }
      else if (STREQ(mode, "volume")) {
        smooth_mode = sculpt_physics::SmoothSolverMode::VolumePreserve;
      }
      else if (STREQ(mode, "relax")) {
        smooth_mode = sculpt_physics::SmoothSolverMode::Relax;
      }
      else if (STREQ(mode, "dense")) {
        smooth_mode = sculpt_physics::SmoothSolverMode::DenseStrong;
      }
      else if (STREQ(mode, "strong") || STREQ(mode, "multiscale")) {
        smooth_mode = sculpt_physics::SmoothSolverMode::DenseStrong;
      }
      else if (STREQ(mode, "physical")) {
        physical_relax = true;
      }
    }
    /* Physical Relax deliberately uses the lightweight XPBD target path and
     * a small amount of rest-shape retention. All dedicated geometric modes
     * keep rest-shape disabled so continuous smoothing can evolve per dab. */
    solver.set_shape_stiffness_override(physical_relax ? 0.08 : 0.0);
    if (smooth_mode == sculpt_physics::SmoothSolverMode::DenseStrong) {
      for (float &weight : compact_weights) {
        weight = std::min(1.0f, weight * 1.75f);
      }
    }
    const char *recovery_mode=std::getenv("BLENDER_SCULPT_D13_SMOOTH_RECOVERY");
    const auto gpu_mode=smooth_mode==sculpt_physics::SmoothSolverMode::DenseStrong &&
        recovery_mode && recovery_mode[0] ?
        sculpt_physics::SmoothSolverMode::DenseStrongRecovery : smooth_mode;
    const bool dedicated_gpu=!physical_relax && solver.smooth_compact(
            active_region, compact_weights, compact_normals, gpu_mode, double(cache.radius));
    if(!dedicated_gpu)
    {
      // A failed A3 launch/readback may already have modified resident
      // positions. Never feed an uncertain buffer into the Jacobi solver.
      // Physical Relax deliberately bypasses A3 and may use Jacobi safely.
      if(!physical_relax){
        throw std::runtime_error("D14 dedicated Smooth trial failed; discard GPU session before native fallback");
      }
      if (!solver.set_active_region(active_region) || !solver.set_targets(targets)) {
        throw std::runtime_error("GPU Smooth rejected its active region");
      }
      solver.step_local(1.0 / 60.0, 1, physical_relax ? 2 : 1, 0);
    }
    if(session->multiscale_dabs++%32==0 || std::getenv("BLENDER_SCULPT_SMOOTH_DIAG"))
      std::fprintf(stderr,"[GPU Smooth %s] mode=%d active=%zu\n",dedicated_gpu?"A3 CUDA":"Jacobi fallback",int(smooth_mode),active_region.size());

    double expected_motion = 0.0;
    double solved_motion = 0.0;
    for (const sculpt_physics::World::BrushTargetInput &target : targets) {
      const float3 current = position_data.eval[int(target.particle_index)];
      expected_motion += math::distance(
          current,
          float3(float(target.goal.x), float(target.goal.y), float(target.goal.z)));
      const sculpt_physics::Vec3 solved = solver.particles()[target.particle_index].position;
      solved_motion += math::distance(
          current, float3(float(solved.x), float(solved.y), float(solved.z)));
    }
    if (!std::isfinite(solved_motion) ||
        (expected_motion > 1.0e-12 && solved_motion <= 1.0e-15))
    {
      /* No Blender positions have been written yet: abandoning the GPU result
       * here is atomic and lets the unchanged standard Smooth execute. */
      throw std::runtime_error("GPU Smooth produced no valid deformation");
    }

    Vector<float3> translations;
    std::vector<unsigned char> written(active_region.size(),0);
    std::vector<std::size_t> correction_ids;
    std::vector<sculpt_physics::Vec3> correction_positions;
    correction_ids.reserve(active_region.size());
    correction_positions.reserve(active_region.size());
    node_mask.foreach_index([&](const int node_i) {
      const Span<int> verts = nodes[node_i].verts();
      translations.resize(verts.size());
      for (const int i : verts.index_range()) {
        const auto target = std::lower_bound(
            targets.begin(),
            targets.end(),
            std::size_t(verts[i]),
            [](const sculpt_physics::World::BrushTargetInput &candidate,
               const std::size_t vertex) { return candidate.particle_index < vertex; });
        if (target == targets.end() || target->particle_index != std::size_t(verts[i])) {
          /* Never publish solver halo/material motion outside the actual brush
           * influence. Doing so exposes rectangular PBVH node boundaries. */
          translations[i] = float3(0.0f);
          continue;
        }
        const auto slot=std::size_t(target-targets.begin());
        if(written[slot]){translations[i]=float3(0.f);continue;}
        written[slot]=1;
        const sculpt_physics::Vec3 p = solver.particles()[verts[i]].position;
        translations[i] = float3(float(p.x) - position_data.eval[verts[i]].x,
                                 float(p.y) - position_data.eval[verts[i]].y,
                                 float(p.z) - position_data.eval[verts[i]].z);
        if (brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE) {
          smooth_preserve_shape_translation(translations[i], normals[verts[i]]);
        }
      }
      const Vector<float3> before_clip=translations;
      clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
      for(const int i:verts.index_range())if(math::is_zero(before_clip[i]))
        translations[i]=float3(0.f);
      position_data.deform(translations, verts);
      bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[node_i]);
    });
    pbvh.tag_positions_changed(node_mask);
    pbvh.flush_bounds_to_parents();
    // Published geometry is authoritative. Never run fallback after write.
    if(dedicated_gpu){
      try{
        for(auto id:active_region){
          const float3 p=position_data.eval[int(id)];
          const auto q=solver.particles()[id].position;
          if(double(p.x)!=q.x || double(p.y)!=q.y || double(p.z)!=q.z){
            correction_ids.push_back(id);
            correction_positions.push_back({double(p.x),double(p.y),double(p.z)});
          }
        }
        if(!correction_ids.empty() &&
           !solver.synchronize_positions_compact(correction_ids,correction_positions,false))
          throw std::runtime_error("accepted position scatter failed");
        if(std::getenv("BLENDER_SCULPT_SMOOTH_DIAG"))
          std::fprintf(stderr,"[D13 Smooth sync] corrected=%zu active=%zu\n",
                       correction_ids.size(),active_region.size());
      }catch(const std::exception &error){
        std::fprintf(stderr,"[D13 Smooth sync] reset after write: %s\n",error.what());
        cache.physical_solver_session.reset();
      }
    }else{
      cache.physical_solver_session.reset();
    }
    return true;
  }
  catch (const std::exception &error) {
    std::fprintf(stderr, "[GPU Smooth A1] fallback to standard Smooth: %s\n", error.what());
    cache.physical_solver_session.reset();
    return false;
  }
}
#endif

BLI_NOINLINE static void do_smooth_brush_mesh(const Depsgraph &depsgraph,
                                              const Sculpt &sd,
                                              const Brush &brush,
                                              Object &object,
                                              const IndexMask &node_mask,
                                              const float brush_strength)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const MeshAttributeData attribute_data(mesh);

  const PositionDeformData position_data(depsgraph, object);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);

  Array<int> node_offset_data;
  const OffsetIndices<int> node_vert_offsets = create_node_vert_offsets(
      nodes, node_mask, node_offset_data);
  Array<float3> new_positions(node_vert_offsets.total_size());
  Array<float> all_factors(node_vert_offsets.total_size());
  Array<float> all_distances(node_vert_offsets.total_size());

  threading::EnumerableThreadSpecific<LocalData> all_tls;

  const auto &delta_state = layer_delta_smooth_v01::state();
  const bool isolate_l0 = delta_state.object == &object && delta_state.mesh_l0 &&
                          delta_state.mesh_reference.size() == position_data.eval.size();
  Array<float3> layer_values;
  if (isolate_l0) { layer_values.reinitialize(position_data.eval.size()); }

  /* Calculate the new positions into a separate array in a separate loop because multiple loops
   * are updated in parallel. Without this there would be non-threadsafe access to changing
   * positions in other bke::pbvh::Tree nodes. */
  for (const float strength : iteration_strengths(brush_strength)) {
    if (isolate_l0) {
      for (const int i : layer_values.index_range()) {
        layer_values[i] = position_data.eval[i] - delta_state.mesh_reference[i];
      }
    }
    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          const Span<int> verts = nodes[i].verts();
          const MutableSpan<float> node_factors = all_factors.as_mutable_span().slice(
              node_vert_offsets[pos]);
          calc_factors_common_mesh_indexed(
              depsgraph,
              brush,
              object,
              attribute_data,
              position_data.eval,
              vert_normals,
              nodes[i],
              node_factors,
              all_distances.as_mutable_span().slice(node_vert_offsets[pos]));
          scale_factors(node_factors, strength);
          const GroupedSpan<int> neighbors = calc_vert_neighbors_interior(
              faces,
              corner_verts,
              vert_to_face_map,
              ss.boundary_info_cache->verts,
              ss.boundary_info_cache->edges,
              attribute_data.hide_poly,
              verts,
              node_factors,
              tls.neighbor_offsets,
              tls.neighbor_data);
          smooth::neighbor_data_average_mesh_check_loose(
              isolate_l0 ? layer_values.as_span() : position_data.eval,
              verts,
              neighbors,
              new_positions.as_mutable_span().slice(node_vert_offsets[pos]));
          if (isolate_l0) {
            MutableSpan<float3> targets = new_positions.as_mutable_span().slice(node_vert_offsets[pos]);
            for (const int j : verts.index_range()) {
              targets[j] += delta_state.mesh_reference[verts[j]];
            }
          }
        },
        exec_mode::grain_size(1));

    node_mask.foreach_index(
        [&](const int i, const int pos) {
          LocalData &tls = all_tls.local();
          apply_positions_faces(sd,
                                brush,
                                vert_normals,
                                nodes[i],
                                object,
                                tls,
                                all_factors.as_mutable_span().slice(node_vert_offsets[pos]),
                                new_positions.as_span().slice(node_vert_offsets[pos]),
                                position_data);
        },
        exec_mode::grain_size(1));
  }
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const OffsetIndices<int> faces,
                       const Span<int> corner_verts,
                       const BitSpan boundary_verts,
                       const Set<OrderedEdge> &boundary_edges,
                       Object &object,
                       const Brush &brush,
                       const float strength,
                       const bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  calc_factors_common_grids(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  scale_factors(tls.factors, strength);

  tls.new_positions.resize(positions.size());
  const MutableSpan<float3> new_positions = tls.new_positions;
  smooth::neighbor_position_average_interior_grids(faces,
                                                   corner_verts,
                                                   boundary_verts,
                                                   boundary_edges,
                                                   subdiv_ccg,
                                                   grids,
                                                   tls.factors,
                                                   new_positions);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_new_positions(new_positions, positions, translations);
  if (brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE) {
    tls.normals.resize(positions.size());
    gather_grids_normals(subdiv_ccg, grids, tls.normals.as_mutable_span());
    for (const int i : translations.index_range()) {
      smooth_preserve_shape_translation(translations[i], tls.normals[i]);
    }
  }
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

/* DELTA_SMOOTH_V01: fixed sparse reference, Jacobi reads followed by separate writes.
 * Reads delta = live CCG - (stroke-start CCG - stitched weighted active-layer delta).
 * Thus no curvature from base/other layers enters the smoothing average. */
/* LAYER_DELTA_SMOOTH_GUARD_V02
 * Mirror clipping may snap a stationary point to its plane. For isolated
 * Smooth, a zero layer delta must stay stationary, including masked samples. */
static void clip_layer_delta_translations(const Sculpt &sd,
                                          const SculptSession &ss,
                                          const Span<float3> positions,
                                          const MutableSpan<float3> translations)
{
  if (!ss.cache || ss.cache->mirror_modifier_clip.flag == 0) {
    clip_and_lock_translations(sd, ss, positions, translations);
    return;
  }
  Vector<int, 64> stationary;
  for (const int i : translations.index_range()) {
    const float3 &delta = translations[i];
    if (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f) {
      stationary.append(i);
    }
  }
  clip_and_lock_translations(sd, ss, positions, translations);
  for (const int i : stationary) {
    translations[i] = float3(0.0f);
  }
}

static bool do_layer_delta_smooth(const Depsgraph &depsgraph,
                                 const Sculpt &sd,
                                 const Brush &brush,
                                 Object &object,
                                 const IndexMask &node_mask,
                                 const float brush_strength)
{
  namespace delta = layer_delta_smooth_v01;
  bke::sculpt_cold_v01::Scope profile("smooth.delta_brush", &object);
  /* Intentional native routing is not a failed Delta Smooth preparation.
   * This also covers Shift and Auto Smooth using the stroke's original choice. */
  if (delta::uses_native_smooth(object)) {
    return false;
  }
  if (!delta::owns(object)) {
    const SculptSession &session = *object.runtime->sculpt_session;
    const MultiresModifierData *mmd = session.multires_modifier;
    if (delta::enabled() && mmd && delta::targets_layer(*mmd)) {
      delta::fail("stroke_not_prepared");
      return true;
    }
    return false;
  }
  if (!delta::prepare_region(object, node_mask)) { return true; }
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(ccg);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  Array<int> offset_data;
  const OffsetIndices<int> offsets = create_node_vert_offsets(key, nodes, node_mask, offset_data);
  Array<float3> changes(offsets.total_size());
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  const auto &reference = delta::state().reference;
  const char *grid_read_env = std::getenv("BLENDER_SCULPT_DELTA_GRID_READ_V01");
  const bool use_grid_read = grid_read_env && grid_read_env[0] == '1' && grid_read_env[1] == '\0';
  const auto read_delta = [&](const SubdivCCGCoord coord) -> float3 {
    const int index = coord.to_index(key);
    return ccg.positions[index] - reference.lookup(index);
  };
  for (const float strength : iteration_strengths(brush_strength)) {
    if (strength <= 0.0f) { continue; }
    node_mask.foreach_index([&](const int i, const int pos) {
      LocalData &tls = all_tls.local();
      const Span<int> grids = nodes[i].grids();
      const MutableSpan positions = gather_grids_positions(ccg, grids, tls.positions);
      calc_factors_common_grids(depsgraph, brush, object, positions, nodes[i],
                                tls.factors, tls.distances);
      scale_factors(tls.factors, strength);
      MutableSpan<float3> node_changes = changes.as_mutable_span().slice(offsets[pos]);
      int local_index = 0;
      for (const int grid : grids) {
        const float3 *grid_reference = use_grid_read ? reference.complete_grid_data(grid) : nullptr;
        const int grid_offset = grid * key.grid_area;
        const auto read_grid_delta = [&](const SubdivCCGCoord coord) -> float3 {
          if (grid_reference && coord.grid_index == grid) {
            const int index = coord.to_index(key);
            return ccg.positions[index] - grid_reference[index - grid_offset];
          }
          return read_delta(coord);
        };
        for (int y = 0; y < key.grid_size; y++) {
          for (int x = 0; x < key.grid_size; x++, local_index++) {
            node_changes[local_index] = float3(0.0f);
            if (tls.factors[local_index] == 0.0f) { continue; }
            const SubdivCCGCoord coord{grid, short(x), short(y)};
            SubdivCCGNeighbors neighbors;
            BKE_subdiv_ccg_neighbor_coords_get(ccg, coord, false, neighbors);
            if (boundary::vert_is_boundary(faces, corner_verts,
                                           ss.boundary_info_cache->verts,
                                           ss.boundary_info_cache->edges, ccg, coord)) {
              if (neighbors.coords.size() == 2) { neighbors.coords.clear(); }
              else {
                neighbors.coords.remove_if([&](const SubdivCCGCoord neighbor) {
                  return !boundary::vert_is_boundary(faces, corner_verts,
                      ss.boundary_info_cache->verts, ss.boundary_info_cache->edges, ccg, neighbor);
                });
              }
            }
            if (neighbors.coords.is_empty()) { continue; }
            const float3 center = read_grid_delta(coord);
            float3 translation(0.0f);
            for (const SubdivCCGCoord neighbor : neighbors.coords) {
              translation += read_grid_delta(neighbor) - center;
            }
            translation /= float(neighbors.coords.size());
            if (brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE) {
              smooth_preserve_shape_translation(translation, ccg.normals[coord.to_index(key)]);
            }
            node_changes[local_index] = translation * tls.factors[local_index];
          }
        }
      }
      clip_layer_delta_translations(sd, ss, positions, node_changes);
    }, exec_mode::grain_size(1));
    /* A barrier separates all neighbor reads from all position writes, including other nodes. */
    node_mask.foreach_index([&](const int i, const int pos) {
      apply_translations(changes.as_span().slice(offsets[pos]), nodes[i].grids(), ccg);
    }, exec_mode::grain_size(1));
  }
  return true;
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float strength,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  calc_factors_common_bmesh(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  scale_factors(tls.factors, strength);

  tls.new_positions.resize(verts.size());
  const MutableSpan<float3> new_positions = tls.new_positions;
  smooth::neighbor_position_average_interior_bmesh(verts, tls.factors, new_positions);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_new_positions(new_positions, positions, translations);
  if (brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE) {
    tls.normals.resize(positions.size());
    gather_bmesh_normals(verts, tls.normals.as_mutable_span());
    for (const int i : translations.index_range()) {
      smooth_preserve_shape_translation(translations[i], tls.normals[i]);
    }
  }
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

}  // namespace smooth_cc

void do_smooth_brush(const Depsgraph &depsgraph,
                     const Sculpt &sd,
                     Object &object,
                     const IndexMask &node_mask,
                     const float brush_strength)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  boundary::ensure_boundary_info(object);
  [[maybe_unused]] bool gpu_selected=false;
#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
  gpu_selected=gpu_smooth_requested() && pbvh.type()==bke::pbvh::Type::Mesh;
  if(gpu_selected){
    if(try_smooth_multiscale_d12(depsgraph,sd,brush,object,node_mask,brush_strength)){
      pbvh.update_bounds(depsgraph,object);return;
    }
    if(flatten_d13_requested()){
      // A flatten rejection is not permission to run a different tool.
      static thread_local unsigned int flatten_rejects=0;
      if(flatten_rejects++%32==0 || std::getenv("BLENDER_SCULPT_SMOOTH_DIAG"))
        std::fprintf(stderr,"[D13 Flatten] unsupported chart or configuration; no write\n");
      return;
    }
    if(multiscale_smooth_requested()){
      static thread_local unsigned int rejects=0;
      if(rejects++%32==0 || std::getenv("BLENDER_SCULPT_SMOOTH_DIAG"))
        std::fprintf(stderr,"[D12 Multiscale] chart unsupported; A3/standard fallback\n");
    }
    if(try_gpu_smooth_a1(depsgraph,sd,brush,object,node_mask,brush_strength)){
      pbvh.update_bounds(depsgraph,object);return;
    }
  }
#endif

#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
  if (!gpu_selected && try_smooth_physics_v15(
          depsgraph,sd,brush,object,node_mask,brush_strength)) {
    pbvh.tag_positions_changed(node_mask);
    pbvh.update_bounds(depsgraph,object);
    return;
  }
#endif

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      do_smooth_brush_mesh(depsgraph, sd, brush, object, node_mask, brush_strength);
      break;
    case bke::pbvh::Type::Grids: {
      if (do_layer_delta_smooth(depsgraph, sd, brush, object, node_mask, brush_strength)) {
        break;
      }
      const Mesh &base_mesh = *id_cast<const Mesh *>(object.data);
      const OffsetIndices faces = base_mesh.faces();
      const Span<int> corner_verts = base_mesh.corner_verts();

      threading::EnumerableThreadSpecific<LocalData> all_tls;
      for (const float strength : iteration_strengths(brush_strength)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        node_mask.foreach_index(
            [&](const int i) {
              LocalData &tls = all_tls.local();
              calc_grids(depsgraph,
                         sd,
                         faces,
                         corner_verts,
                         ss.boundary_info_cache->verts,
                         ss.boundary_info_cache->edges,
                         object,
                         brush,
                         strength,
                         nodes[i],
                         tls);
            },
            exec_mode::grain_size(1));
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      vert_random_access_ensure(object);
      threading::EnumerableThreadSpecific<LocalData> all_tls;
      for (const float strength : iteration_strengths(brush_strength)) {
        MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
        node_mask.foreach_index(
            [&](const int i) {
              LocalData &tls = all_tls.local();
              calc_bmesh(depsgraph, sd, object, brush, strength, nodes[i], tls);
            },
            exec_mode::grain_size(1));
      }
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.update_bounds(depsgraph, object);
}

}  // namespace blender::ed::sculpt_paint::brushes

