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
#include <map>
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
#  include "d15_feature_smooth.hh"
#  include "d15_uv_boundary.hh"
#  include "d15_surface_anchor.hh"
#  include "editors/sculpt_paint/mesh/sculpt_coverage_diag_d15.hh"
#  include "editors/sculpt_paint/mesh/sculpt_undo.hh"
#  include "BLI_vector_set.hh"
#  include "BKE_attribute.hh"
#  include "BKE_mesh_mapping.hh"
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

/* D15 CPU pilot, deliberately independent of the existing Smooth modes.
 * Only actual PBVH brush vertices may move; adjacent samples are read-only.
 * Edge tags are derived from current native attributes each dab, so changing
 * a Face Set or explicit seam does not require a separate dirty cache.
 * Keep UI hidden until native verification and CUDA parity. */
static bool feature_smooth_d15_requested()
{
  const char *enabled=std::getenv("BLENDER_SCULPT_D15_FEATURE_SMOOTH");
  return enabled && STREQ(enabled,"1");
}

/* C4/C6: stage UV proposals from the old incident triangle fan.
 * Only interior vertices with one continuous wedge across all maps are
 * eligible. Probe never publishes; preserve requires a complete valid dab. */
static bool feature_uv_probe_d15(const Mesh &mesh,
                                const Span<float3> positions,
                                const Span<float3> normals,
                                const std::unordered_map<std::size_t,float3> &changes,
                                Vector<undo::UVCornerProposalD15> *proposals,
                                std::vector<std::size_t> *rejected_vertices=nullptr,
                                const bool sculpt_normal_transport=false,
                                const bool adaptive_wedges=false)
{
  namespace a=sculpt_physics::surface_anchor_d15;
  const auto names=mesh.uv_map_names();
  if(names.is_empty() || names.size()>16 || changes.size()>4096){
    std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=scope layers=%lld vertices=%zu\n",
                 static_cast<long long>(names.size()),changes.size());
    return false;
  }
  const auto attrs=mesh.attributes();
  const auto faces=mesh.faces();
  const auto v2f=mesh.vert_to_face_map();
  const auto cv=mesh.corner_verts();
  const auto tris=mesh.corner_tris();
  const auto as_v=[](const float3 &v)->a::V{return {double(v.x),double(v.y),double(v.z)};};
  std::vector<VArray<float2>> layers;
  std::vector<std::string> layer_names;
  for(const auto name:names){
    auto r=attrs.lookup<float2>(name,bke::AttrDomain::Corner);
    if(!r || r.varray.size()!=cv.size())return false;
    layers.push_back(std::move(r.varray));
    layer_names.emplace_back(name.data(),name.size());
  }
  std::size_t projected=0,refused=0,corner_values=0;
  std::size_t normal_reject=0,fan_reject=0,residual_reject=0,uv_reject=0,normal_only=0;
  double max_uv_delta=0.,max_normal_motion=0.,max_tangent_motion=0.;
  std::size_t wedge_count=0,multi_wedge_count=0;
  for(const auto &entry:changes){
    const auto reject=[&](std::size_t &reason){
      ++refused;++reason;
      if(rejected_vertices)rejected_vertices->push_back(entry.first);
    };
    const int v=int(entry.first);
    const auto motion=as_v(entry.second);
    const double full_travel=a::norm(motion);
    if(!std::isfinite(full_travel)){reject(normal_reject);continue;}
    if(!(full_travel>0.))continue;
    const auto old=as_v(positions[v]);
    const auto normal=as_v(normals[v]);
    const double nl=a::norm(normal);
    if(!(nl>0.) || !std::isfinite(nl)){reject(normal_reject);continue;}
    const auto n=a::mul(normal,1./nl);
    const double normal_motion=a::dot(motion,n);
    // C7 separates surface redistribution from material deformation.
    // The full guarded position is published; only its tangential component
    // samples the old surface. Pure normal motion keeps the original UVs.
    if(!sculpt_normal_transport && std::abs(normal_motion)>full_travel*1e-4){
      reject(normal_reject);continue;
    }
    const auto tangent=sculpt_normal_transport?
        a::add(motion,a::mul(n,-normal_motion)):motion;
    const double travel=a::norm(tangent);
    if(!std::isfinite(travel)){reject(normal_reject);continue;}
    max_normal_motion=std::max(max_normal_motion,std::abs(normal_motion));
    max_tangent_motion=std::max(max_tangent_motion,travel);
    // float32 publication and contact scaling introduce rounding in a pure
    // normal vector. This tolerance is relative, not a world-space bypass.
    if(sculpt_normal_transport && travel<=full_travel*1e-6){++normal_only;continue;}
    const auto target=a::add(old,tangent);
    struct Candidate {int tri;int3 corners;a::V bary;double distance;};
    std::vector<Candidate> hits;
    std::vector<int> dest;
    bool bad=false;
    for(const int face:v2f[v]){
      for(const int c:faces[face])if(cv[c]==v)dest.push_back(c);
      for(const int ti:bke::mesh::face_triangles_range(faces,face)){
        const int3 c=tris[ti];
        if(cv[c[0]]!=v && cv[c[1]]!=v && cv[c[2]]!=v)continue;
        if(hits.size()>=256){bad=true;break;}
        a::Triangle t;
        t.id=std::uint64_t(ti);
        for(int j=0;j<3;++j){t.vertices[j]=std::uint64_t(cv[c[j]]);t.co[j]=as_v(positions[cv[c[j]]]);}
        a::V tn,bary;double distance=0.;
        if(!a::triangle_normal(t,tn) || a::dot(tn,n)<.5 || !a::closest(t,target,bary,distance)){
          bad=true;break;
        }
        hits.push_back({ti,c,bary,distance});
      }
      if(bad || dest.size()>256){bad=true;break;}
    }
    if(bad || hits.empty() || dest.empty()){reject(fan_reject);continue;}
    // Require one connected triangle fan around the vertex, even when two
    // disconnected fans happen to share the same point and UV coordinates.
    std::vector<unsigned char> seen(hits.size(),0);seen[0]=1;
    std::vector<std::size_t> queue{0};
    for(std::size_t k=0;k<queue.size();++k)for(std::size_t j=0;j<hits.size();++j){
      if(seen[j])continue;
      int common=0;
      for(int x=0;x<3;++x)for(int y=0;y<3;++y)
        if(cv[hits[queue[k]].corners[x]]==cv[hits[j].corners[y]])++common;
      if(common==2){seen[j]=1;queue.push_back(j);}
    }
    if(queue.size()!=hits.size()){reject(fan_reject);continue;}
    std::sort(hits.begin(),hits.end(),[](const Candidate &x,const Candidate &y){
      return x.distance!=y.distance?x.distance<y.distance:x.tri<y.tri;
    });
    const auto &best=hits.front();
    if(!adaptive_wedges && best.distance>travel*.25){reject(residual_reject);continue;}
    // All maps use the same geometric correspondence. Inspect every
    // destination corner and reject incompatible equidistant UV samples.
    double local_delta=0.;
    Vector<undo::UVCornerProposalD15> local;
    for(std::size_t li=0;li<layers.size();++li){
      const auto &layer=layers[li];
      const auto read=[&](int c)->a::UV{const float2 uv=layer[c];return {double(uv.x),double(uv.y)};};
      std::vector<std::vector<std::size_t>> groups;
      if(adaptive_wedges){
        std::vector<a::Triangle> fan;
        fan.reserve(hits.size());
        for(const auto &hit:hits){
          a::Triangle tri;tri.id=std::uint64_t(hit.tri);
          for(int k=0;k<3;++k){
            const int c=hit.corners[k];tri.vertices[k]=std::uint64_t(cv[c]);
            tri.co[k]=as_v(positions[cv[c]]);tri.uv[k]=read(c);
          }
          fan.push_back(tri);
        }
        if(!a::partition_wedges(fan,1e-6,groups)){bad=true;break;}
      }else{
        groups.emplace_back();
        for(std::size_t k=0;k<hits.size();++k)groups.back().push_back(k);
      }
      std::vector<int> assigned(dest.size(),-1);
      const auto sample=[&](const Candidate &h,a::UV &out){
        out={};
        for(int j=0;j<3;++j){const auto uv=read(h.corners[j]);if(!a::finite(uv))return false;
          for(int u=0;u<2;++u)out[u]+=h.bary[j]*uv[u];}
        return a::finite(out);
      };
      for(std::size_t gi=0;gi<groups.size();++gi){
        const auto &group=groups[gi];
        if(group.empty()){bad=true;break;}
        const auto &winner=hits[group.front()];
        if(winner.distance>travel*.25){bad=true;break;}
        std::vector<int> corners;
        if(!adaptive_wedges)corners=dest;
        else{
          for(const auto k:group)for(int z=0;z<3;++z){
            const int c=hits[k].corners[z];if(cv[c]==v)corners.push_back(c);
          }
          std::sort(corners.begin(),corners.end());
          corners.erase(std::unique(corners.begin(),corners.end()),corners.end());
        }
        if(corners.empty()){bad=true;break;}
        const auto original=read(corners.front());
        if(!a::finite(original)){bad=true;break;}
        for(const int c:corners){
          const auto it=std::find(dest.begin(),dest.end(),c);
          if(it==dest.end()){bad=true;break;}
          const auto k=std::size_t(it-dest.begin());
          if(assigned[k]!=-1 || !a::finite(read(c)) || !a::same_uv(read(c),original,1e-6)){
            bad=true;break;
          }
          assigned[k]=int(gi);
        }
        if(bad)break;
        a::UV proposed;
        if(!sample(winner,proposed)){bad=true;break;}
        for(const auto k:group){
          const auto &hit=hits[k];
          if(hit.distance-winner.distance>std::max(1e-12,travel*1e-6))break;
          a::UV other;
          if(!sample(hit,other)||!a::same_uv(other,proposed,1e-6)){bad=true;break;}
        }
        if(bad)break;
        local_delta=std::max(local_delta,std::hypot(proposed[0]-original[0],proposed[1]-original[1]));
        if(proposals){
          const std::array<float,2> after{float(proposed[0]),float(proposed[1])};
          if(!std::isfinite(after[0])||!std::isfinite(after[1])){bad=true;break;}
          for(const int c:corners){
            const float2 before=layer[c];
            if(before.x!=after[0]||before.y!=after[1])local.append({layer_names[li],c,after});
          }
        }
      }
      if(bad||std::find(assigned.begin(),assigned.end(),-1)!=assigned.end()){bad=true;break;}
      if(adaptive_wedges){wedge_count+=groups.size();if(groups.size()>1)++multi_wedge_count;}
    }
    if(bad){reject(uv_reject);continue;}
    if(proposals){
      if(proposals->size()+local.size()>262144)return false;
      for(const auto &value:local)proposals->append(value);
    }
    ++projected;corner_values+=dest.size()*layers.size();
    max_uv_delta=std::max(max_uv_delta,local_delta);
  }
  std::fprintf(stderr,"[D15 UV %s] layers=%zu projected=%zu refused=%zu corner_values=%zu max_uv_delta=%.9g\n",
               proposals?"STAGE":"Probe READ ONLY",layers.size(),projected,refused,corner_values,max_uv_delta);
  if(sculpt_normal_transport)
    std::fprintf(stderr,"[D15 UV Sculpt Transport] normal_only=%zu max_normal=%.9g max_tangent=%.9g\n",
                 normal_only,max_normal_motion,max_tangent_motion);
  if(adaptive_wedges)
    std::fprintf(stderr,"[D15 C8 Wedges] sampled_groups=%zu multi_wedge_layer_vertices=%zu\n",wedge_count,multi_wedge_count);
  if(refused)
    std::fprintf(stderr,"[D15 UV Reject] normal=%zu fan=%zu residual=%zu uv=%zu\n",
                 normal_reject,fan_reject,residual_reject,uv_reject);
  return !proposals || rejected_vertices || refused==0;
}

/* C6 guarded writer set. Allocation and UV triangle checks happen before
 * journal capture and before changing positions or UV values. */
class FeatureUVPublishD15 {
  using UV=std::array<float,2>;
  struct Layer {
    std::string name;
    std::map<int,UV> values;
    std::unique_ptr<bke::SpanAttributeWriter<float2>> writer;
  };
  std::vector<Layer> layers_;
public:
  ~FeatureUVPublishD15(){
    for(auto &l:layers_)if(l.writer && *l.writer)l.writer->finish();
  }
  bool prepare(Mesh &mesh,Span<undo::UVCornerProposalD15> proposals,
               const std::unordered_map<std::size_t,float3> &changes,
               std::vector<std::size_t> &bad_vertices)
  {
    std::map<std::string,std::map<int,UV>> grouped;
    for(const auto &p:proposals){
      if(p.corner<0||p.corner>=mesh.corners_num)return false;
      auto &map=grouped[p.layer];
      const auto r=map.emplace(p.corner,p.after);
      if(!r.second&&r.first->second!=p.after)return false;
    }
    const auto faces=mesh.faces();
    const auto tris=mesh.corner_tris();
    const auto cv=mesh.corner_verts();
    const auto v2f=mesh.vert_to_face_map();
    std::vector<int> affected;
    for(const auto &entry:changes)
      for(const int face:v2f[int(entry.first)])affected.push_back(face);
    std::sort(affected.begin(),affected.end());
    affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
    layers_.reserve(grouped.size());
    for(auto &[name,values]:grouped){
      if(!bke::mesh::is_uv_map(mesh.attributes().lookup_meta_data(name)))return false;
      auto writer=std::make_unique<bke::SpanAttributeWriter<float2>>(
          mesh.attributes_for_write().lookup_for_write_span<float2>(name));
      layers_.push_back({name,std::move(values),std::move(writer)});
      auto &l=layers_.back();
      const auto &w=*l.writer;
      if(!w||w.domain!=bke::AttrDomain::Corner||w.span.size()!=mesh.corners_num)return false;
      const auto area=[](const std::array<UV,3> &t){
        return (double(t[1][0])-t[0][0])*(double(t[2][1])-t[0][1])-
               (double(t[1][1])-t[0][1])*(double(t[2][0])-t[0][0]);
      };
      for(const int face:affected)for(const int ti:bke::mesh::face_triangles_range(faces,face)){
        const int3 c=tris[ti];
        std::array<UV,3> old{},next{};
        bool changed=false;
        for(int j=0;j<3;++j){
          const float2 v=w.span[c[j]];
          old[j]={v.x,v.y};next[j]=old[j];
          const auto it=l.values.find(c[j]);
          if(it!=l.values.end()){next[j]=it->second;changed=changed||next[j]!=old[j];}
        }
        if(!changed)continue;
        const double a=area(old),b=area(next);
        if(!std::isfinite(a)||!std::isfinite(b)||a==0.||
           ((a>0.)!=(b>0.))||std::abs(b)<.05*std::abs(a)){
          // Only endpoints whose corner UV actually changed can cause
          // the invalid UV triangle. Normal-only material motion keeps UVs.
          for(int j=0;j<3;++j){
            const auto v=std::size_t(cv[c[j]]);
            if(old[j]!=next[j] && changes.find(v)!=changes.end())bad_vertices.push_back(v);
          }
        }
      }
    }
    return true;
  }
  void write_values(){
    for(auto &l:layers_)for(const auto &[corner,value]:l.values)
      l.writer->span[corner]=float2(value[0],value[1]);
  }
  void finish(bke::pbvh::Tree &pbvh,const IndexMask &nodes){
    for(auto &l:layers_){
      l.writer->finish();
      l.writer.reset();
      pbvh.tag_attribute_changed(nodes,l.name);
    }
  }
};

static bool try_feature_smooth_d15(const Depsgraph &depsgraph,
                                  const Sculpt &sd,
                                  const Brush &brush,
                                  Object &object,
                                  const IndexMask &node_mask,
                                  const float brush_strength)
{
  namespace fs=sculpt_physics::feature_smooth_d15;
  namespace uvb=sculpt_physics::uv_boundary_d15;
  const char *probe_flag=std::getenv("BLENDER_SCULPT_D15_UV_PROBE");
  const bool uv_probe=probe_flag && STREQ(probe_flag,"1");
  const char *preserve_flag=std::getenv("BLENDER_SCULPT_D15_UV_PRESERVE");
  const bool uv_preserve=!uv_probe && preserve_flag && STREQ(preserve_flag,"1");
  const char *tangent_flag=std::getenv("BLENDER_SCULPT_D15_TANGENT_ONLY");
  const char *sculpt_flag=std::getenv("BLENDER_SCULPT_D15_UV_SCULPT");
  const bool uv_sculpt=!uv_probe && sculpt_flag && STREQ(sculpt_flag,"1");
  const bool tangent_only=!uv_sculpt &&
      (uv_probe || uv_preserve || (tangent_flag && STREQ(tangent_flag,"1")));
  const bool constrained_features=tangent_only||uv_sculpt;
  const char *adaptive_flag=std::getenv("BLENDER_SCULPT_D15_ADAPTIVE");
  const bool adaptive=uv_sculpt&&adaptive_flag&&STREQ(adaptive_flag,"1");
  const char *faceset_flag=std::getenv("BLENDER_SCULPT_D15_PROTECT_FACE_SETS");
  const bool protect_facesets=!adaptive||!faceset_flag||!STREQ(faceset_flag,"0");
  SculptSession &ss=*object.runtime->sculpt_session;
  StrokeCache &cache=*ss.cache;
  bke::pbvh::Tree &pbvh=*bke::object::pbvh_get(object);
  const bool diag=std::getenv("BLENDER_SCULPT_SMOOTH_DIAG")!=nullptr;
  const auto reject=[&](const char *reason){
    if(diag)std::fprintf(stderr,"[D15 C6.2 BLOCK] reason=%s preserve=%d probe=%d tangent=%d\n",
                        reason,int(uv_preserve),int(uv_probe),int(tangent_only));
    return false;
  };
  const auto &layer=layer_delta_smooth_v01::state();
  if(pbvh.type()!=bke::pbvh::Type::Mesh)return reject("pbvh_not_mesh");
  if(ss.deform_modifiers_active)return reject("deform_modifiers_active");
  if(ss.shapekey_active)return reject("shapekey_active");
  if(bke::multires_layers::base_modifier(object)!=nullptr)return reject("multires_base_modifier");
  if(layer.object==&object && layer.mesh_l0)return reject("isolated_layer_mesh_l0");
  if(cache.reference_symmetry)return reject("reference_symmetry");
  if(cache.reference_combined)return reject("reference_combined");
  if(cache.tile_pass!=0 || cache.radial_symmetry_pass!=0 || cache.mirror_symmetry_pass!=0){
    if(diag)std::fprintf(stderr,"[D15 C6.2 PASS] tile=%d radial=%d mirror=%d\n",
                         int(cache.tile_pass),int(cache.radial_symmetry_pass),int(cache.mirror_symmetry_pass));
    return reject("non_main_pass");
  }
  const Mesh &mesh=*id_cast<const Mesh *>(object.data);
  if(stroke_symmetry_xyz_get(object)!=PAINT_SYMM_NONE){
    if(diag)std::fprintf(stderr,"[D15 C6.2 SYMM] xyz=%d\n",int(stroke_symmetry_xyz_get(object)));
    return reject("xyz_symmetry");
  }
  if(mesh.radial_symmetry[0]!=1 || mesh.radial_symmetry[1]!=1 || mesh.radial_symmetry[2]!=1){
    if(diag)std::fprintf(stderr,"[D15 C6.2 SYMM] radial=%d,%d,%d\n",
                         int(mesh.radial_symmetry[0]),int(mesh.radial_symmetry[1]),int(mesh.radial_symmetry[2]));
    return reject("radial_symmetry");
  }
  if((sd.paint.symmetry_flags & (PAINT_TILE_X | PAINT_TILE_Y | PAINT_TILE_Z))!=0)
    return reject("tile_symmetry");
  const PositionDeformData pos(depsgraph,object);
  if(pos.eval.size()!=mesh.verts_num)return reject("position_count_mismatch");
  if(!std::isfinite(brush_strength))return reject("nonfinite_strength");
  const MeshAttributeData attrs(mesh);
  const Span<float3> normals=bke::pbvh::vert_normals_eval(depsgraph,object);
  if(normals.size()!=pos.eval.size())return reject("normal_count_mismatch");
  if(diag)std::fprintf(stderr,"[D15 C7 ENTER] preserve=%d probe=%d tangent=%d sculpt=%d verts=%d strength=%.6g\n",
                       int(uv_preserve),int(uv_probe),int(tangent_only),int(uv_sculpt),mesh.verts_num,double(brush_strength));
  const auto faces=mesh.faces();
  const auto v2f=mesh.vert_to_face_map();
  const auto native_edges=mesh.edges();
  const auto corner_edges=mesh.corner_edges();
  const auto corner_verts=mesh.corner_verts();
  MutableSpan<bke::pbvh::MeshNode> nodes=pbvh.nodes<bke::pbvh::MeshNode>();
  struct WriteBatch {int node=0;Vector<int> ids;Vector<float3> motion;};
  std::vector<WriteBatch> batches;
  Vector<undo::UVCornerProposalD15> uv_proposals;
  std::unique_ptr<FeatureUVPublishD15> uv_publish;
  fs::Result result;
  std::size_t active=0,edge_count=0,feature_count=0,blocked_count=0;
  std::size_t uv_layers=0,uv_splits=0,uv_invalid=0,uv_pinned_count=0;
  std::size_t geometry_pinned=0,seam_normal_count=0,clip_pinned=0,normal_recovered=0,uv_removed=0;
  double guard_scale=1.;
  const auto start=std::chrono::steady_clock::now();
  try{
    std::unordered_map<int,std::size_t> slot;
    std::vector<int> global;
    std::vector<fs::Vertex> samples;
    std::vector<unsigned char> feature,blocked,uv_pinned,uv_normal,hard_feature;
    const auto cv=[](const float3 &v)->fs::V{return {double(v.x),double(v.y),double(v.z)};};
    const auto visible=[&](int id){return attrs.hide_vert.is_empty() || !attrs.hide_vert[id];};
    const auto add=[&](int id,double weight){
      const auto found=slot.find(id);
      if(found!=slot.end()){
        auto &x=samples[found->second];
        x.weight=std::max(x.weight,weight);
        return found->second;
      }
      const auto i=samples.size();
      slot.emplace(id,i);global.push_back(id);
      samples.push_back({cv(pos.eval[id]),cv(normals[id]),weight,visible(id)});
      feature.push_back(0);blocked.push_back(0);uv_pinned.push_back(0);uv_normal.push_back(0);hard_feature.push_back(0);
      return i;
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
      for(const int j:verts.index_range()){
        const float factor=tls.factors[j];
        if(!std::isfinite(factor)){rejected=true;return;}
        if(factor<=0.f || !visible(verts[j]))continue;
        // C7 boosts only the selected Smooth pilot, not native brush modes.
        add(verts[j],std::clamp(double(factor)*(uv_sculpt?1.75:1.),0.,1.));
        if(samples.size()>500000){rejected=true;return;}
      }
    });
    if(rejected)return reject("invalid_factor_or_active_cap");
    active=samples.size();
    if(active==0){if(diag)std::fprintf(stderr,"[D15 C6.2 NOOP] reason=no_active_vertices\n");return true;}
    if(ss.vert_to_edge_map.is_empty() && !native_edges.is_empty()){
      ss.vert_to_edge_map=bke::mesh::build_vert_to_edge_map(
          native_edges,mesh.verts_num,ss.vert_to_edge_offsets,ss.vert_to_edge_indices);
    }
    if(native_edges.is_empty()){if(diag)std::fprintf(stderr,"[D15 C6.2 NOOP] reason=no_edges\n");return true;}
    if(ss.vert_to_edge_map.size()!=mesh.verts_num)return reject("edge_map_count_mismatch");
    std::vector<int> edge_ids;
    std::vector<int> incident_faces;
    for(std::size_t i=0;i<active;++i){
      const int v=global[i];
      for(const int e:ss.vert_to_edge_map[v])edge_ids.push_back(e);
      for(const int face:v2f[v]){
        incident_faces.push_back(face);
        if(!attrs.hide_poly.is_empty() && attrs.hide_poly[face])blocked[i]=1;
      }
    }
    std::sort(edge_ids.begin(),edge_ids.end());
    edge_ids.erase(std::unique(edge_ids.begin(),edge_ids.end()),edge_ids.end());
    std::sort(incident_faces.begin(),incident_faces.end());
    incident_faces.erase(std::unique(incident_faces.begin(),incident_faces.end()),incident_faces.end());
    if(edge_ids.size()>4000000)return reject("edge_cap");
    struct EdgeInfo {
      int count=0,first_set=0,first_face=-1;
      bool mixed=false,hidden=false;
      std::array<uvb::Corners,2> corners{};
      uvb::Status uv=uvb::Status::Continuous;
    };
    std::vector<EdgeInfo> info(edge_ids.size());
    std::unordered_map<int,std::size_t> edge_slot;
    edge_slot.reserve(edge_ids.size());
    for(std::size_t i=0;i<edge_ids.size();++i)edge_slot.emplace(edge_ids[i],i);
    for(const int face:incident_faces){
      const bool hidden=!attrs.hide_poly.is_empty() && attrs.hide_poly[face];
      const int set=attrs.face_sets.is_empty()?0:attrs.face_sets[face];
      const IndexRange face_corners=faces[face];
      for(const int corner:face_corners){
        const int e=corner_edges[corner];
        const auto it=edge_slot.find(e);
        if(it==edge_slot.end())continue;
        auto &q=info[it->second];
        if(q.count==0){q.first_set=set;q.first_face=face;}
        else if(q.first_set!=set)q.mixed=true;
        if(q.count<2){
          const int next=corner==face_corners.last()?face_corners.first():corner+1;
          const int2 endpoints=native_edges[e];
          if((q.count==1 && q.first_face==face) ||
             !uvb::ordered_corners(endpoints[0],endpoints[1],
                                   corner_verts[corner],corner_verts[next],
                                   corner,next,q.corners[q.count])){
            q.uv=uvb::Status::Invalid;
          }
        }
        ++q.count;q.hidden=q.hidden||hidden;
      }
    }
    const auto attributes=mesh.attributes();
    // Read every native UV map, not just the active one. Corner values are
    // matched by vertex identity; opposite face winding cannot fake a seam.
    // No persistent UV cache or attribute write in this pilot.
    const auto uv_names=mesh.uv_map_names();
    for(const auto name:uv_names){
      const auto reader=attributes.lookup<float2>(name,bke::AttrDomain::Corner);
      if(!reader || reader.varray.size()!=corner_verts.size())return reject("invalid_uv_layer");
      ++uv_layers;
      const auto sample_pair=[&](const uvb::Corners &ids)->uvb::Pair{
        const float2 a=reader.varray[ids[0]],b=reader.varray[ids[1]];
        return {uvb::UV{double(a.x),double(a.y)},uvb::UV{double(b.x),double(b.y)}};
      };
      for(auto &q:info){
        if(q.count<1 || q.count>2 || q.uv==uvb::Status::Invalid)continue;
        const auto a=sample_pair(q.corners[0]);
        const auto status=q.count==2?uvb::classify(a,sample_pair(q.corners[1])):
            (uvb::valid(a)?uvb::Status::Continuous:uvb::Status::Invalid);
        q.uv=uvb::combine(q.uv,status);
      }
    }
    const auto sharp=*attributes.lookup_or_default<bool>("sharp_edge",bke::AttrDomain::Edge,false);
    const auto seams=*attributes.lookup_or_default<bool>("uv_seam",bke::AttrDomain::Edge,false);
    std::vector<fs::Edge> edges;
    edges.reserve(edge_ids.size());
    for(std::size_t i=0;i<edge_ids.size();++i){
      const int e=edge_ids[i];const int2 v=native_edges[e];
      if(v[0]==v[1])return reject("self_edge");
      const auto a=add(v[0],0.),b=add(v[1],0.);
      if(samples.size()>1000000)return reject("sample_cap");
      const auto &q=info[i];
      unsigned flags=0;
      if(sharp[e])flags|=fs::Sharp;
      if(seams[e])flags|=fs::Seam;
      if(q.mixed&&protect_facesets)flags|=fs::FaceSet;
      if(q.count==1)flags|=fs::Open;
      if(q.uv==uvb::Status::Split){
        flags|=fs::UVChart;
        // A true UV discontinuity is pinned until chart-aware reprojection
        // and corner-attribute Undo exist. Sliding now could stretch texture.
        uv_pinned[a]=uv_pinned[b]=1;
        ++uv_splits;
      }
      if(q.uv==uvb::Status::Invalid)++uv_invalid;
      const bool unsafe=q.count==0||q.count>2||q.hidden||q.uv==uvb::Status::Invalid;
      if(unsafe){blocked[a]=1;blocked[b]=1;}
      // A UV chart boundary is not necessarily a geometric feature.
      // C7 normal material transport may smooth a UV-only seam without
      // changing either wedge; real sharp/open/Face Set features remain pinned.
      if(uv_sculpt && !adaptive && (flags&(fs::Seam|fs::UVChart))){
        uv_normal[a]=uv_normal[b]=1;
        flags&=~unsigned(fs::Seam|fs::UVChart);
      }
      if(flags&(fs::Sharp|fs::Open|fs::Guide))hard_feature[a]=hard_feature[b]=1;
      if(flags){feature[a]=feature[b]=1;++feature_count;}
      edges.push_back({a,b,flags,!unsafe});
    }
    edge_count=edges.size();
    for(std::size_t i=0;i<samples.size();++i){
      if(samples[i].weight<=0.)continue;
      if(uv_pinned[i]&&(!uv_sculpt||blocked[i]||feature[i]))++uv_pinned_count;
      if(blocked[i])++blocked_count;
      if(blocked[i]||(!uv_sculpt&&uv_pinned[i]))samples[i].weight=0.;
    }
    fs::Settings settings;
    if(tangent_only)settings.normal_keep=0.;
    if(uv_sculpt){settings.normal_keep=1.;settings.max_step=.35;}
    settings.adaptive_boundaries=adaptive;
    if(constrained_features&&!adaptive){
      for(std::size_t i=0;i<samples.size();++i)if(feature[i]){
        if(samples[i].weight>0.)++geometry_pinned;
        samples[i].weight=0.;
      }
    }
    if(!fs::evaluate(samples,std::move(edges),settings,result))return reject("feature_evaluate");
    std::unordered_map<std::size_t,float3> changes;
    changes.reserve(active);
    std::vector<unsigned char> staged(active,0);
    bool adjusted_by_clip=false;
    node_mask.foreach_index([&](const int ni){
      WriteBatch batch;batch.node=ni;
      for(const int id:nodes[ni].verts()){
        const auto it=slot.find(id);
        if(it==slot.end()||it->second>=active||staged[it->second])continue;
        const auto i=it->second;staged[i]=1;
        auto d=result.motion[i];
        if(uv_sculpt && uv_normal[i] && samples[i].weight>0.){
          fs::V normal_d{};
          if(!fs::normal_component(d,samples[i].normal,normal_d)){rejected=true;return;}
          d=normal_d;
          if(fs::length(d)>0.)++seam_normal_count;
        }
        float3 v{float(d[0]),float(d[1]),float(d[2])};
        // Feature motion already has its own tangent constraint; the generic
        // normal attenuation would rotate it away from that constraint.
        if((!feature[i]||(adaptive&&!hard_feature[i])) && (brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE))
          smooth_preserve_shape_translation(v,normals[id]);
        if(math::length_squared(v)>0.f){batch.ids.append(id);batch.motion.append(v);}
      }
      if(batch.ids.is_empty())return;
      const Vector<float3> before=batch.motion;
      clip_and_lock_translations(sd,ss,pos.eval,batch.ids,batch.motion);
      for(const int j:batch.ids.index_range()){
        auto &v=batch.motion[j];
        const bool changed=v.x!=before[j].x||v.y!=before[j].y||v.z!=before[j].z;
        const auto i=slot.at(batch.ids[j]);
        if(changed && (feature[i]||constrained_features)){
          v=float3(0.f);++clip_pinned; // retain native clip/lock contract
        }
        else if(changed)adjusted_by_clip=true;
        if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)){rejected=true;return;}
        if(math::length_squared(v)>0.f)changes.emplace(std::size_t(batch.ids[j]),v);
      }
      batches.push_back(std::move(batch));
    });
    if(rejected)return reject("invalid_clipped_motion");
    if(changes.empty()){
      if(uv_probe||uv_preserve)std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=no_eligible_motion active=%zu feature_edges=%zu\n",active,feature_count);
      return true;
    }
    if(!uv_preserve){
      guard_scale=smooth_local_contact_guard(mesh,pos.eval,changes);
      if(!std::isfinite(guard_scale)||guard_scale<0.||guard_scale>1.)return reject("invalid_geometry_guard");
      // Do not partially scale a clipping snap/lock correction.
      if(adjusted_by_clip && guard_scale<1.)return reject("clip_guard_conflict");
    }
    if(uv_probe)return feature_uv_probe_d15(mesh,pos.eval,normals,changes,nullptr);
    if(uv_preserve){
      // A single unsafe vertex must not freeze an otherwise safe dab.
      // Prune locally, re-run the geometric guard and regenerate EVERY UV
      // proposal from the original snapshot after each rejection round.
      const auto original_changes=changes;
      auto remaining=original_changes;
      std::vector<unsigned char> tried_normal(active,0);
      bool ready=false;
      constexpr int max_rounds=6;
      for(int round=0;round<max_rounds;++round){
        changes=remaining;
        if(changes.empty())break;
        const double pass_scale=smooth_local_contact_guard(mesh,pos.eval,changes);
        if(!std::isfinite(pass_scale)||pass_scale<0.||pass_scale>1.){
          std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=invalid_geometry_guard\n");
          return false;
        }
        if(adjusted_by_clip&&pass_scale<1.)return reject("preserve_clip_guard_conflict");
        for(auto it=changes.begin();it!=changes.end();){
          const auto v=it->second;
          if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))return reject("nonfinite_motion");
          if(math::length_squared(v)==0.f)it=changes.erase(it);else ++it;
        }
        if(changes.empty()){
          std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=zero_guarded_motion scale=%.9g\n",pass_scale);
          break;
        }
        std::vector<std::size_t> bad;
        uv_proposals.clear();
        if(!feature_uv_probe_d15(mesh,pos.eval,normals,changes,&uv_proposals,&bad,uv_sculpt,adaptive)){
          std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=projection_preflight\n");
          return false;
        }
        if(bad.empty()){
          auto writer=std::make_unique<FeatureUVPublishD15>();
          Mesh &writable_mesh=*id_cast<Mesh *>(object.data);
          if(!writer->prepare(writable_mesh,uv_proposals.as_span(),changes,bad)){
            std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=writer_preflight\n");
            return false;
          }
          if(bad.empty()){
            uv_publish=std::move(writer);
            guard_scale=pass_scale;
            ready=true;break;
          }
          // Writer destructs before next read/guard pass; no UV was written.
        }
        std::sort(bad.begin(),bad.end());
        bad.erase(std::unique(bad.begin(),bad.end()),bad.end());
        std::size_t removed=0,recovered=0;
        for(const auto id:bad){
          auto it=remaining.find(id);
          if(it==remaining.end())continue;
          const auto i=slot.at(int(id));
          if(uv_sculpt && i<active && !tried_normal[i] && !(adaptive?hard_feature[i]:feature[i]) && !blocked[i]){
            tried_normal[i]=1;
            // Do not discard safe shape smoothing solely because tangent
            // UV correspondence failed. Keep the ORIGINAL clipped normal
            // component, re-check native constraints, then re-run ALL
            // geometry and UV guards on the new complete candidate set.
            const float3 before=it->second;
            fs::V normal_d{};
            if(fs::normal_component(cv(before),samples[i].normal,normal_d)){
              float3 nmove{float(normal_d[0]),float(normal_d[1]),float(normal_d[2])};
              const float3 candidate=nmove;
              const int vertex=int(id);
              if(math::length_squared(candidate)>0.f &&
                 (candidate.x!=before.x||candidate.y!=before.y||candidate.z!=before.z)){
                clip_and_lock_translations(sd,ss,pos.eval,Span<int>(&vertex,1),
                                           MutableSpan<float3>(&nmove,1));
                if(nmove.x==candidate.x&&nmove.y==candidate.y&&nmove.z==candidate.z){
                  it->second=nmove;++recovered;continue;
                }
              }
            }
          }
          removed+=remaining.erase(id);
        }
        normal_recovered+=recovered;uv_removed+=removed;
        std::fprintf(stderr,"[D15 UV Preserve FILTER] round=%d removed=%zu recovered_normal=%zu remaining=%zu\n",
                     round+1,removed,recovered,remaining.size());
        if(!removed&&!recovered)break;
      }
      if(!ready){
        std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=no_safe_subset\n");
        return false;
      }
    }
    for(auto &batch:batches)for(const int j:batch.ids.index_range()){
      const auto it=changes.find(std::size_t(batch.ids[j]));
      batch.motion[j]=it==changes.end()?float3(0.f):it->second;
      const auto v=batch.motion[j];
      if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))return reject("nonfinite_motion");
    }
    if(uv_preserve){
      // Every position writer must belong to a captured native Undo node.
      for(const auto &batch:batches)
        if(!orig_position_data_lookup_mesh(object,nodes[batch.node])){
          std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=position_undo_node\n");
          return false;
        }
      // Capture last: all proposals and writers have been acquired. No
      // fallback after this point. Only ordinary Mesh without deform/shape/
      // Multires is supported by this opt-in experimental path.
      if(!undo::capture_uv_corners_d15(object,uv_proposals.as_span())){
        std::fprintf(stderr,"[D15 UV Preserve BLOCK] reason=uv_undo_capture\n");
        return false;
      }
    }
  }catch(const std::exception &error){
    std::fprintf(stderr,"[D15 Feature Smooth] rejected before write: %s\n",error.what());
    return false;
  }
  // Existing native position Undo transaction covers node_mask. No fallback
  // or alternate mode after any publish; GPU state is stale after CPU writes.
  for(auto &batch:batches)pos.deform(batch.motion,batch.ids);
  if(uv_preserve)uv_publish->write_values();
  for(auto &batch:batches)bke::pbvh::update_node_bounds_mesh(pos.eval,nodes[batch.node]);
  if(uv_preserve){
    IndexMaskMemory uv_memory;
    const IndexMask uv_nodes=bke::pbvh::all_leaf_nodes(pbvh,uv_memory);
    uv_publish->finish(pbvh,uv_nodes);
    std::fprintf(stderr,"[D15 UV Preserve COMMIT] corner_values=%lld guard=%.4f\n",
                 static_cast<long long>(uv_proposals.size()),guard_scale);
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  cache.physical_solver_session.reset();
  if(std::getenv("BLENDER_SCULPT_SMOOTH_DIAG")){
    std::fprintf(stderr,"[D15 Feature Smooth CPU] active=%zu edges=%zu feature_edges=%zu sliding=%zu pinned=%zu blocked=%zu guard=%.4f ms=%.3f\n",
                 active,edge_count,feature_count,result.sliding,result.pinned,blocked_count,guard_scale,
                 std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    std::fprintf(stderr,"[D15 UV Boundary] layers=%zu split_edges=%zu invalid_edges=%zu pinned_vertices=%zu\n",
                 uv_layers,uv_splits,uv_invalid,uv_pinned_count);
    if(adaptive)std::fprintf(stderr,"[D15 C8 Adaptive] sliding=%zu pinned=%zu protect_facesets=%d\n",result.sliding,result.pinned,int(protect_facesets));
    if(uv_sculpt)std::fprintf(stderr,"[D15 C7.1 Coverage] active=%zu moved=%zu seam_normal=%zu normal_recovered=%zu uv_removed=%zu geometry_pinned=%zu blocked=%zu clip_pinned=%zu\n",
                              active,changes.size(),seam_normal_count,normal_recovered,uv_removed,geometry_pinned,blocked_count,clip_pinned);
  }
  return true;
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
  const char *c9_flag=std::getenv("BLENDER_SCULPT_D15_GPU_PRESERVE");
  const bool c9_requested=c9_flag&&STREQ(c9_flag,"1");
  bool c9_publish_started=false;
  const char *coverage_flag=std::getenv("BLENDER_SCULPT_D15_COVERAGE_DIAG");
  const bool coverage=coverage_flag&&STREQ(coverage_flag,"1")&&
      mesh.verts_num<=250000&&cache.radius>0.f;
  struct CoverageVertex {
    float distance=0.f,factor=0.f;
    float3 before=float3(0.f);
    float native_goal=0.f,gpu_goal=0.f,clipped_goal=0.f;
    bool gathered=false,submitted=false,staged=false;
  };
  std::vector<CoverageVertex> audit;
  const auto watched=coverage?coverage_diag_d15::watched_vertices():std::vector<int>{};
  if(coverage){
    Array<float> distances(mesh.verts_num);
    calc_brush_distances(ss,position_data.eval,eBrushFalloffShape(brush.falloff_shape),
                         distances.as_mutable_span());
    audit.resize(mesh.verts_num);
    for(const int v:position_data.eval.index_range()){
      audit[v].distance=distances[v];
      audit[v].before=position_data.eval[v];
    }
  }
  const auto report_coverage=[&](const char *phase){
    if(!coverage)return;
    std::size_t core=0,hidden=0,masked=0,missing=0,zero_factor=0,submitted=0,
        zero_gpu=0,zero_clipped=0,unwritten=0,moved=0,flat_goal=0;
    int samples=0;
    const float epsilon=std::max(cache.radius*1e-6f,1e-12f);
    for(const int v:position_data.eval.index_range()){
      const auto &x=audit[v];
      // The central 75% of the actual native 3D/tube footprint, not
      // the projected outline of the cursor. Back-side hits may be
      // deliberately excluded by Front Faces Only.
      if(!std::isfinite(x.distance)||x.distance>=cache.radius*.75f)continue;
      ++core;
      const float mask=attributes.mask.is_empty()?0.f:attributes.mask[v];
      const bool hide=!attributes.hide_vert.is_empty()&&attributes.hide_vert[v];
      const float moved_distance=math::distance(position_data.eval[v],x.before);
      const float ndot=math::dot(cache.view_normal_symm,normals[v]);
      const char *reason=nullptr;
      if(hide){++hidden;continue;}
      if(mask>=.999f){++masked;continue;}
      if(!x.gathered){++missing;reason="not_gathered";}
      else if(!x.submitted){++zero_factor;reason="factor_zero_or_invalid";}
      else{
        ++submitted;
        if(!x.staged){++unwritten;reason="not_staged";}
        else if(moved_distance>epsilon){++moved;}
        else if(x.gpu_goal<=epsilon){
          if(x.native_goal<=epsilon){++flat_goal;reason="near_zero_native_and_gpu_goal";}
          else{++zero_gpu;reason="gpu_goal_zero";}
        }else if(x.clipped_goal<=epsilon){++zero_clipped;reason="post_clip_or_shape_zero";}
        else{++unwritten;reason="staged_not_moved";}
      }
      if(reason&&samples<12){
        ++samples;
        std::fprintf(stderr,"[D15 C9.1 Vertex] id=%d reason=%s co=%.7g,%.7g,%.7g dist_ratio=%.5g factor=%.6g mask=%.5g view_dot=%.5g native_goal=%.7g gpu_goal=%.7g clipped=%.7g actual=%.7g\n",
                     v,reason,double(x.before.x),double(x.before.y),double(x.before.z),
                     double(x.distance/cache.radius),double(x.factor),double(mask),double(ndot),
                     double(x.native_goal),double(x.gpu_goal),double(x.clipped_goal),double(moved_distance));
      }
    }
    // Record every watched ID, including those outside the actual brush
    // radius. Do not mistake a screen-space overlap for 3D influence.
    for(const int v:watched){
      if(v<0||v>=mesh.verts_num){
        std::fprintf(stderr,"[D15 C9.2 Watch] phase=%s id=%d reason=id_out_of_range\n",phase,v);
        continue;
      }
      const auto &x=audit[v];
      const float mask=attributes.mask.is_empty()?0.f:attributes.mask[v];
      const bool hide=!attributes.hide_vert.is_empty()&&attributes.hide_vert[v];
      const float actual=math::distance(position_data.eval[v],x.before);
      const float ndot=math::dot(cache.view_normal_symm,normals[v]);
      const char *reason=hide?"hidden":mask>=.999f?"masked":
          !std::isfinite(x.distance)?"invalid_distance":
          x.distance>=cache.radius?"outside_radius":
          !x.gathered?"not_gathered":
          !x.submitted?"not_submitted":
          !x.staged?"not_staged":
          actual>epsilon?"moved":
          x.gpu_goal<=epsilon?(x.native_goal<=epsilon?"both_goals_small":"gpu_goal_small"):
          x.clipped_goal<=epsilon?"post_clip_or_shape_small":"staged_not_moved";
      std::fprintf(stderr,"[D15 C9.2 Watch] phase=%s id=%d reason=%s dist_ratio=%.7g gathered=%d submitted=%d staged=%d factor=%.7g mask=%.7g view_dot=%.7g native_goal=%.9g gpu_goal=%.9g clipped=%.9g actual=%.9g co=%.9g,%.9g,%.9g\n",
                   phase,v,reason,double(x.distance/cache.radius),int(x.gathered),int(x.submitted),
                   int(x.staged),double(x.factor),double(mask),double(ndot),
                   double(x.native_goal),double(x.gpu_goal),double(x.clipped_goal),double(actual),
                   double(x.before.x),double(x.before.y),double(x.before.z));
    }
    std::fprintf(stderr,"[D15 C9.1 Coverage] core=%zu hidden=%zu masked=%zu missing=%zu zero_factor=%zu submitted=%zu flat_goal=%zu zero_gpu=%zu zero_clip=%zu unwritten=%zu moved=%zu nodes=%lld frontface=%d falloff=%d strength=%.6g eps=%.7g\n",
                 core,hidden,masked,missing,zero_factor,submitted,flat_goal,zero_gpu,zero_clipped,
                 unwritten,moved,static_cast<long long>(node_mask.size()),int(bool(brush.flag&BRUSH_FRONTFACE)),
                 int(brush.falloff_shape),double(brush_strength),double(epsilon));
  };

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
        if(coverage){
          auto &x=audit[verts[i]];
          x.gathered=true;
          x.factor=std::max(x.factor,tls.factors[i]);
          x.native_goal=math::distance(position_data.eval[verts[i]],tls.new_positions[i]);
        }
        if (tls.factors[i] > 0.0f && std::isfinite(tls.factors[i])) {
          gathered.push_back({verts[i], tls.factors[i], tls.new_positions[i]});
        }
      }
    });
    if (gathered.empty()) {
      report_coverage("empty_gather");
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
      if(coverage)audit[target.vertex].submitted=true;
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
      const float distance=math::distance(
          current, float3(float(solved.x), float(solved.y), float(solved.z)));
      solved_motion += distance;
      if(coverage)audit[target.particle_index].gpu_goal=distance;
    }
    if (!std::isfinite(solved_motion) ||
        (expected_motion > 1.0e-12 && solved_motion <= 1.0e-15))
    {
      /* No Blender positions have been written yet: abandoning the GPU result
       * here is atomic and lets the unchanged standard Smooth execute. */
      report_coverage("gpu_no_deformation");
      throw std::runtime_error("GPU Smooth produced no valid deformation");
    }

    Vector<float3> translations;
    // C9: collect the exact legacy GPU displacements (including native
    // Preserve Shape and clipping) before any publication. Attribute
    // preservation may change corner UVs, never the accepted displacement.
    const bool c9=c9_requested&&dedicated_gpu&&
        smooth_mode==sculpt_physics::SmoothSolverMode::DenseStrong;
    struct C9Batch {int node;Vector<float3> motion;};
    std::vector<C9Batch> c9_batches;
    std::unordered_map<std::size_t,float3> c9_changes;
    if(c9)c9_changes.reserve(active_region.size());
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
      if(coverage){
        for(const int i:verts.index_range()){
          auto &x=audit[verts[i]];
          if(x.submitted){
            x.staged=true;
            x.clipped_goal=std::max(x.clipped_goal,math::length(translations[i]));
          }
        }
      }
      if(c9){
        for(const int i:verts.index_range()){
          const auto v=translations[i];
          if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))
            throw std::runtime_error("C9 nonfinite GPU displacement");
          if(!math::is_zero(v))c9_changes.emplace(std::size_t(verts[i]),v);
        }
        c9_batches.push_back({node_i,translations});
      }else{
        position_data.deform(translations, verts);
        bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[node_i]);
      }
    });
    if(c9){
      Vector<undo::UVCornerProposalD15> proposals;
      std::unique_ptr<FeatureUVPublishD15> uv_writer;
      IndexMaskMemory uv_memory;
      IndexMask uv_nodes;
      std::size_t material_vertices=0;
      const char *material_reason=nullptr;
      // Do not extend the C5 corner Undo scope by assumption. Outside it,
      // keep original UVs and run the already supported GPU geometry path.
      const bool scope=!cache.reference_symmetry&&!cache.reference_combined&&
          cache.mirror_symmetry_pass==0&&stroke_symmetry_xyz_get(object)==PAINT_SYMM_NONE&&
          mesh.radial_symmetry[0]==1&&mesh.radial_symmetry[1]==1&&mesh.radial_symmetry[2]==1&&
          (sd.paint.symmetry_flags&(PAINT_TILE_X|PAINT_TILE_Y|PAINT_TILE_Z))==0;
      if(!scope)material_reason="unsupported_uv_scope";
      else if(c9_changes.empty())material_reason="no_motion";
      else if(mesh.uv_map_names().is_empty())material_reason="no_uv_maps";
      else{
        // A failed attribute correspondence must never carve a stationary
        // island into the smoothing result. Drop only the UV proposals of
        // that vertex (all maps/wedges), not its geometric displacement.
        // This is explicit material transport, not successful reprojection.
        std::vector<std::size_t> rejected;
        if(!feature_uv_probe_d15(mesh,position_data.eval,normals,c9_changes,
                                 &proposals,&rejected,true,true)){
          material_reason="projection_preflight";
        }else{
          const auto cv=mesh.corner_verts();
          constexpr int max_uv_rounds=6;
          bool ready=false;
          std::sort(rejected.begin(),rejected.end());
          rejected.erase(std::unique(rejected.begin(),rejected.end()),rejected.end());
          material_vertices=rejected.size();
          for(int round=0;round<max_uv_rounds;++round){
            Vector<undo::UVCornerProposalD15> filtered;
            for(const auto &q:proposals){
              if(q.corner<0||q.corner>=cv.size())
                throw std::runtime_error("C9 invalid proposed corner");
              if(!std::binary_search(rejected.begin(),rejected.end(),std::size_t(cv[q.corner])))
                filtered.append(q);
            }
            proposals=std::move(filtered);
            if(proposals.is_empty()){ready=true;break;}
            std::vector<std::size_t> bad;
            auto writer=std::make_unique<FeatureUVPublishD15>();
            if(!writer->prepare(mesh,proposals.as_span(),c9_changes,bad)){
              material_reason="writer_preflight";break;
            }
            if(bad.empty()){
              uv_writer=std::move(writer);ready=true;break;
            }
            // Revalidate all remaining UV triangles after removing these
            // corners. Geometry is identical in every iteration.
            rejected.insert(rejected.end(),bad.begin(),bad.end());
            std::sort(rejected.begin(),rejected.end());
            rejected.erase(std::unique(rejected.begin(),rejected.end()),rejected.end());
            material_vertices=rejected.size();
          }
          if(!ready&&!material_reason)material_reason="uv_guard_budget";
        }
      }
      if(uv_writer&&!material_reason){
        for(const auto &batch:c9_batches){
          if(!orig_position_data_lookup_mesh(object,nodes[batch.node])){
            material_reason="position_undo_node";break;
          }
        }
        if(!material_reason){
          uv_nodes=bke::pbvh::all_leaf_nodes(pbvh,uv_memory);
          if(!undo::capture_uv_corners_d15(object,proposals.as_span()))
            material_reason="uv_undo_capture";
        }
      }
      if(material_reason){
        uv_writer.reset();
        proposals.clear();
        material_vertices=c9_changes.size();
      }
      // The exact same displacements as legacy A3 are authoritative.
      // No allocation-dependent staging or fallback after journal capture.
      c9_publish_started=true;
      for(auto &batch:c9_batches){
        position_data.deform(batch.motion,nodes[batch.node].verts());
      }
      if(uv_writer)uv_writer->write_values();
      for(const auto &batch:c9_batches)
        bke::pbvh::update_node_bounds_mesh(position_data.eval,nodes[batch.node]);
      if(uv_writer)uv_writer->finish(pbvh,uv_nodes);
      std::fprintf(stderr,"[D15 C9 GPU Preserve] active=%zu moved=%zu uv_corners=%lld material_vertices=%zu reason=%s\n",
                   active_region.size(),c9_changes.size(),static_cast<long long>(proposals.size()),
                   material_vertices,material_reason?material_reason:"local_transport");
    }else if(c9_requested){
      std::fprintf(stderr,"[D15 C9 GPU Preserve] inactive: requires dedicated DenseStrong GPU\n");
    }
    report_coverage("published");
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
    if(c9_publish_started){
      // A failure after publication cannot authorize a second Smooth.
      std::fprintf(stderr,"[D15 C9] publication error; no second deformation: %s\n",error.what());
      cache.physical_solver_session.reset();
      pbvh.tag_positions_changed(node_mask);
      pbvh.flush_bounds_to_parents();
      return true;
    }
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
  if(feature_smooth_d15_requested()){
    if(try_feature_smooth_d15(depsgraph,sd,brush,object,node_mask,brush_strength)){
      pbvh.update_bounds(depsgraph,object);
    }else if(std::getenv("BLENDER_SCULPT_SMOOTH_DIAG")){
      std::fprintf(stderr,"[D15 Feature Smooth] unsupported or rejected; no write\n");
    }
    return; // explicit pilot never silently substitutes another Smooth
  }
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

