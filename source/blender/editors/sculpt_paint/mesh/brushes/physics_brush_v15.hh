/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* V15 opt-in multi-brush XPBD preview. This small Blender-only bridge
 * consumes FINAL stock brush translations (pressure, mask, falloff, planes,
 * automasking and texture are already resolved by each brush). The isolated
 * CPU physics executor only modifies their compliant target/edge response.
 * NEVER include/use it in standard builds unless the V14 build switch is on.
 *
 * Still research: no Blender full-source build, true native Undo rollback
 * on a partially applied node batch, Multires CCG or CUDA has been verified.
 */
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15

#  include "native_grab_executor_adapter.hh"
#  include "compact_native_brush_adapter.hh"
#  include "physical_brush_intent.hh"
#  include "clay_surface_solver.hh"
#  include "material_transport_v21.hh"
#  include "material_profiles_v25.hh"
#  include "brush_contact.hh"
#  include "global_collision_v29.hh"
#  include "stroke_context_v27.hh"

#  include "BKE_mesh.hh"
#  include "BKE_attribute.hh"
#  include "editors/sculpt_paint/mesh/sculpt_undo.hh"
#  include "BKE_brush.hh"
#  include "BKE_colortools.hh"
#  include "BKE_paint.hh"
#  include "BKE_mesh_types.hh"
#  include "DNA_brush_types.h"
#  include "BKE_multires_sculpt_layers.hh"
#  include "BKE_object_types.hh"
#  include "BKE_paint_bvh.hh"
#  include "DNA_scene_types.h"
#  include "BLI_math_vector.hh"

#  include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#  include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#  include <algorithm>
#  include <cmath>
#  include <chrono>
#  include <cstdio>
#  include <cstdlib>
#  include <memory>
#  include <stdexcept>
#  include <unordered_map>
#  include <unordered_set>
#  include <utility>
#  include <vector>

namespace blender::ed::sculpt_paint::brushes::physics_v15 {

struct NativePhysicsStroke {
  sculpt_physics::World rest_world;
  std::unique_ptr<sculpt_physics::GpuTopologyBondIndex> topology;
  std::shared_ptr<const sculpt_physics::CompactBrushTopology> compact;
  sculpt_physics::CompactRegionCacheV27 region_cache;
  std::unordered_map<std::size_t, sculpt_physics::Vec3> original_positions;
  const Mesh *mesh_identity = nullptr;
  const int2 *edge_data = nullptr;
  int verts_num = 0;
  int edges_num = 0;
  std::size_t autonomous_dabs = 0;
  double autonomous_peak_sum = 0.0;
  double autonomous_peak = 0.0;
};

inline bool fast_v26b_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_TOOL_V26B");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool stroke_v27_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_STROKE_V27");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool contact_v27b_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_CONTACT_V27B");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool optimize_v27c_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_OPTIMIZE_V27C");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool stable_v28_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_STABLE_V28");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
struct SurfaceContactRefV28 { sculpt_physics::Vec3 position{}; double radius=0; };
using SurfaceContactHistoryV28=std::unordered_map<std::size_t,SurfaceContactRefV28>;
inline bool refine_v28b_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_REFINE_V28B");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool live_surface_v30_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_LIVE_SURFACE_V30");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
/* Only supported explicit physical Clay-family assets. Do not change stock
 * brushes, nonaccumulating anchored stamps, or unsupported geometry modes. */
inline bool live_surface_v30_brush(const Object &object,const Brush &brush)
{
  if(!live_surface_v30_requested() ||
     (brush.sculpt_physics_mode!=2 && brush.sculpt_physics_mode!=3) ||
     brush.sculpt_physics_assist<=0 ||
     (brush.sculpt_brush_type!=SCULPT_BRUSH_TYPE_CLAY &&
      brush.sculpt_brush_type!=SCULPT_BRUSH_TYPE_CLAY_STRIPS) ||
     (brush.stroke_method!=BRUSH_STROKE_SPACE && brush.stroke_method!=BRUSH_STROKE_AIRBRUSH))
    return false;
  const auto *ss=object.runtime->sculpt_session;
  const auto *pbvh=bke::object::pbvh_get(object);
  return ss && pbvh && pbvh->type()==bke::pbvh::Type::Mesh &&
         !ss->deform_modifiers_active && !ss->shapekey_active &&
         bke::multires_layers::base_modifier(const_cast<Object &>(object))==nullptr;
}
inline bool transport_v30b_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_TRANSPORT_V30B");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool contact_v30b_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_CONTACT_V30B");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool overlap_v30c_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_OVERLAP_V30C");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool push_v30d_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_PUSH_V30D");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool average_surface_v31d_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_AVERAGE_V31D");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool continuous_contact_v31d2_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_CONTINUOUS_V31D2");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled && average_surface_v31d_requested();
}
inline bool motion_proxy_v31d3_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_PROXY_V31D3");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled && continuous_contact_v31d2_requested();
}
inline bool average_transport_action_v31d(int action)
{
  return action==3 || action==6 || action==11 || action==12 || action==13;
}
inline bool gradient_v31c_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_GRADIENT_V31C");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool melt_fallback_v31c_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_MELT_FALLBACK_V31C");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool adaptive_v31_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_ADAPTIVE_V31");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool round_material_action_v31(int action)
{
  // Asset ids 2/3 map to Compress/Push (5/6) in surface_v20.
  return action==2 || action==3 || (action>=5 && action<=14);
}
inline bool direct_material_action_v31(int action)
{
  return action==2 || action==5 || action==7 || action==8 || action==14;
}
inline bool swept_v29_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_SWEEP_V29");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
// Explicit material sphere brush, normal Mesh, short supported segment.
// Exclude Deposit and native assisted Clay (different footprint geometry).
inline bool swept_v29_active(const StrokeCache &cache,const Brush &brush)
{
  if(!swept_v29_requested() || !stroke_v27_requested() || !stable_v28_requested() ||
     !refine_v28b_requested() ||
     brush.sculpt_physics_mode!=3 || brush.sculpt_physics_assist<=0 ||
     brush.falloff_shape!=PAINT_FALLOFF_SHAPE_SPHERE ||
     !round_material_action_v31(brush.sculpt_physics_clay_behavior) ||
     !cache.xpbd_v17_intent_started || cache.xpbd_v15_failed ||
     cache.reference_symmetry || cache.reference_combined ||
     cache.tile_pass!=0 || cache.radial_symmetry_pass!=0 || cache.mirror_symmetry_pass!=0)
    return false;
  const double distance=math::length(cache.location_symm-cache.xpbd_v17_last_center);
  return std::isfinite(distance) && cache.radius>0 && distance>1e-12 && distance<=cache.radius*.5;
}
inline float swept_distance_v29(const StrokeCache &cache,float3 p,const float3 *axis=nullptr)
{
  float3 a=cache.xpbd_v17_last_center-cache.location_symm;
  p-=cache.location_symm;
  if(axis){a-=*axis*math::dot(a,*axis);p-=*axis*math::dot(p,*axis);}
  const float den=math::dot(a,a);
  const float t=den>1e-20f?std::clamp(math::dot(p,a)/den,0.0f,1.0f):0.0f;
  return math::length(p-a*t);
}
inline bool footprint_v30e_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_FOOTPRINT_V30E");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool footprint_audit_v30e_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_FOOTPRINT_AUDIT_V30E");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool footprint_v30e_brush(const Object &object,const Brush &brush)
{
  return footprint_v30e_requested() && live_surface_v30_brush(object,brush) &&
         stroke_v27_requested() && stable_v28_requested() && refine_v28b_requested() &&
         brush.sculpt_physics_mode==3 && brush.falloff_shape==PAINT_FALLOFF_SHAPE_SPHERE &&
         round_material_action_v31(brush.sculpt_physics_clay_behavior);
}
inline float footprint_distance_v30e(const StrokeCache &cache,const Brush &brush,
                                      const float3 &p,const float3 &normal)
{
  const float3 axis=math::normalize(normal);
  if(swept_v29_active(cache,brush))return swept_distance_v29(cache,p,&axis);
  const float3 delta=p-cache.location_symm;
  return math::length(delta-axis*math::dot(delta,axis));
}
struct FootprintAuditV30E {
  std::size_t geometric=0,base=0,front=0,trim=0,curve=0,automask=0,texture=0;
  static std::size_t count(Span<float> factors,Span<float> distances,float radius)
  {
    std::size_t total=0;
    for(const int i:factors.index_range())if(distances[i]<radius && factors[i]>0)++total;
    return total;
  }
  void report(int action) const
  {
    std::fprintf(stderr,"[V30E factors] action=%d geometric=%zu mask_clip=%zu front=%zu trim=%zu curve=%zu automask=%zu texture=%zu\n",
                 action,geometric,base,front,trim,curve,automask,texture);
  }
};
inline bool plastic_v29_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_PLASTIC_V29");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool global_collision_v29_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_COLLISION_V29");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}
inline bool profile_v26b_requested()
{
  static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_PROFILE_V26");return v && v[0]=='1' && v[1]=='\0';}();
  return enabled;
}

// Process-level experimental tuning; immutable for the full stroke.
// Zero (or invalid explicit input) bypasses V15 entirely, preserving the
// ordinary Blender evaluator including its own clipping and layer paths.
inline double assist_influence()
{
  static const double value=[]() {
    const char *raw=std::getenv("BLENDER_SCULPT_XPBD_V15_ASSIST");
    if (!raw) return 1.0; // retain previous V15 behavior
    char *end=nullptr;
    const double result=std::strtod(raw,&end);
    if (end==raw || *end!='\0' || !std::isfinite(result) ||
        result<0.0 || result>1.0) return 0.0;
    return result;
  }();
  return value;
}

inline bool compact_requested()
{
  static const bool value=[]() {
    const char *raw=std::getenv("BLENDER_SCULPT_XPBD_V16_COMPACT");
    return raw && raw[0]=='1' && raw[1]=='\0';
  }();
  return value;
}

// Explicit experimental process overrides. Missing/invalid values keep
// the selected brush profile. No dynamic UI or physical calibration implied.
inline double material_override(const char *name, double fallback, double lo, double hi)
{
  const char *raw=std::getenv(name);
  if (!raw || !raw[0]) return fallback;
  char *end=nullptr;
  const double value=std::strtod(raw,&end);
  return end!=raw && *end=='\0' && std::isfinite(value) && value>=lo && value<=hi ?
             value : fallback;
}

inline bool requested(Object &object,const Brush &brush)
{
  StrokeCache &cache=*object.runtime->sculpt_session->cache;
  if (cache.xpbd_v16_settings_initialized) return cache.xpbd_v16_enabled;
  cache.xpbd_v16_settings_initialized=true;
  if (brush.sculpt_physics_mode==1) return false;
  const bool explicit_mode=brush.sculpt_physics_mode==2 || brush.sculpt_physics_mode==3;
  const char *value=std::getenv("BLENDER_SCULPT_XPBD_OTHER_BRUSHES_V15");
  const char *material=std::getenv("BLENDER_SCULPT_XPBD_V16_CLAY_MATERIAL");
  cache.xpbd_v16_assist=explicit_mode ? double(brush.sculpt_physics_assist) : assist_influence();
  cache.xpbd_v16_enabled=(explicit_mode || (value && value[0]=='1' && value[1]=='\0')) &&
      std::isfinite(cache.xpbd_v16_assist) && cache.xpbd_v16_assist>0.0 &&
      cache.xpbd_v16_assist<=1.0;
  cache.xpbd_v16_compact=explicit_mode || compact_requested();
  cache.xpbd_v16_clay_material=explicit_mode ? brush.sculpt_physics_mode==3 :
      (material && material[0]=='1' && material[1]=='\0');
  if (explicit_mode) {
    cache.xpbd_v16_clay_behavior=brush.sculpt_physics_clay_behavior;
    cache.xpbd_v16_clay_yield=brush.sculpt_physics_yield;
    cache.xpbd_v16_clay_plasticity=brush.sculpt_physics_plasticity;
    cache.xpbd_v16_clay_spread=brush.sculpt_physics_spread;
  }
  return cache.xpbd_v16_enabled;
}

/* If Blender's geometry mode changes after this physical brush has
 * already applied any dab, DO NOT mix in stock brush's different evaluator.
 * Return true to finish this stroke without changing geometry. */
inline bool tool_v25_requested();

inline bool freeze_if_mode_changed(Object &object, const bke::pbvh::Tree &pbvh,const Brush &brush)
{
  if (!requested(object,brush) || pbvh.type()==bke::pbvh::Type::Mesh) return false;
  StrokeCache &cache=*object.runtime->sculpt_session->cache;
  cache.xpbd_v15_failed=true;
  const bool strict_material=brush.sculpt_physics_mode==3 &&
      (tool_v25_requested() || brush.sculpt_physics_clay_behavior==14);
  if (strict_material && !cache.xpbd_v15_active_logged)
    std::fprintf(stderr,"[V25] Material tools require supported Mesh topology; no stock fallback.\n");
  return strict_material || cache.xpbd_v15_active_logged;
}

inline bool drag_contact_requested()
{
  static const bool enabled=[]() {
    const char *v=std::getenv("BLENDER_SCULPT_CONTACT_V19");
    return v && v[0]=='1' && v[1]=='\0';
  }();
  return enabled;
}

// Evaluate all triangles incident on actual staged write IDs, including
// quad diagonals and pinned neighboring vertices. Never scan all faces.
inline sculpt_physics::LocalContactResult limit_drag_contact(
    const Mesh &mesh, const Span<float3> positions,
    std::unordered_map<std::size_t,float3> &changes, const bool uniform_only,
    const bool balance_local=false,const bool coherent_v31b=false)
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
  double coherent=1.0,legacy_scale=1.0;
  const bool gradient=coherent_v31b && gradient_v31c_requested();
  if(coherent_v31b){
    // Check the full accumulated native dab, not only solver substeps.
    if(!gradient || profile_v26b_requested())legacy_scale=sculpt_physics::coherent_motion_scale_v31b(p,triangles,motion);
    coherent=gradient?sculpt_physics::gradient_motion_scale_v31c(p,triangles,motion):legacy_scale;
    if(coherent<1)for(auto &v:motion)v=v*coherent;
  }
  auto result=balance_local && !coherent_v31b?sculpt_physics::limit_balanced_contact_v30c(p,triangles,motion):
                                 sculpt_physics::limit_local_contact(p,triangles,motion,uniform_only || coherent_v31b);
  result.final_scale*=coherent;result.minimum_scale*=coherent;
  if(gradient && profile_v26b_requested())
    std::fprintf(stderr,"[V31C gradient] legacy_scale=%.7g gradient_scale=%.7g final_scale=%.7g triangles=%zu\n",legacy_scale,coherent,result.final_scale,triangles.size());
  if(coherent_v31b && profile_v26b_requested())
    std::fprintf(stderr,"[V31B guard] coherent_scale=%.7g final_scale=%.7g triangles=%zu\n",coherent,result.final_scale,triangles.size());
  for(std::size_t i=0;i<global.size();++i) {
    auto it=changes.find(global[i]);
    if(it!=changes.end()) {
      it->second=float3(float(motion[i].x),float(motion[i].y),float(motion[i].z));
    }
  }
  return result;
}

inline bool try_apply(const Sculpt &sd,
                      Object &object,
                      const IndexMask &node_mask,
                      const PositionDeformData &position_data,
                      const sculpt_physics::NativeSculptBrushKind kind,
                      const std::vector<sculpt_physics::NativeBrushTranslation> &native_translations,
                      const bool autonomous=false,
                      const bool conservative_clay=false,
                      const sculpt_physics::ClaySurfaceDab *plastic_dab=nullptr)
{
  SculptSession &ss=*object.runtime->sculpt_session;
  StrokeCache &cache=*ss.cache;
  if (!cache.xpbd_v16_enabled) return false;
  if (cache.xpbd_v15_failed) return conservative_clay || cache.xpbd_v15_active_logged;
  bke::pbvh::Tree &pbvh=*bke::object::pbvh_get(object);
  const Mesh &mesh=*id_cast<Mesh *>(object.data);
  // Check the actual dispatcher configuration BEFORE its first pass.
  // Object mirror axes are not stored in Paint::symmetry_flags in this fork.
  const bool unsupported_symmetry =
      stroke_symmetry_xyz_get(object)!=PAINT_SYMM_NONE ||
      cache.reference_symmetry || cache.reference_combined ||
      mesh.radial_symmetry[0]!=1 || mesh.radial_symmetry[1]!=1 ||
      mesh.radial_symmetry[2]!=1 ||
      (sd.paint.symmetry_flags & (PAINT_TILE_X | PAINT_TILE_Y | PAINT_TILE_Z))!=0;
  if (pbvh.type()!=bke::pbvh::Type::Mesh ||
      ss.deform_modifiers_active || ss.shapekey_active ||
      bke::multires_layers::base_modifier(object)!=nullptr ||
      unsupported_symmetry ||
      cache.tile_pass!=0 || cache.radial_symmetry_pass!=0 ||
      cache.mirror_symmetry_pass!=0) {
    // A previously active physical stroke must not switch abruptly to the
    // stock brush; freeze remaining dabs until the user releases the stroke.
    // Once a stroke chose native fallback, never enter the physical solver
    // later in that same stroke if the geometry configuration changes.
    std::fprintf(stderr,
                 "[V16 physical brush] fallback: pbvh=%d deform=%d shape=%d multires=%d symmetry=%d tile=%d radial=%d mirror=%d\n",
                 int(pbvh.type()),int(ss.deform_modifiers_active),int(ss.shapekey_active != nullptr),
                 int(bke::multires_layers::base_modifier(object)!=nullptr),
                 int(unsupported_symmetry),int(cache.tile_pass),
                 int(cache.radial_symmetry_pass),int(cache.mirror_symmetry_pass));
    cache.xpbd_v15_failed=true;
    return (conservative_clay && (tool_v25_requested() || cache.xpbd_v16_clay_behavior==14)) ||
           cache.xpbd_v15_active_logged;
  }
  if (mesh.verts_num<=0 || position_data.eval.size()!=mesh.verts_num) {
    cache.xpbd_v15_failed=true;
    return cache.xpbd_v15_active_logged;
  }

  MutableSpan<bke::pbvh::MeshNode> nodes=pbvh.nodes<bke::pbvh::MeshNode>();
  const Span<float3> current=position_data.eval;
  struct PreparedNode {
    int id;
    Vector<float3> translations;
  };
  std::vector<PreparedNode> editor_writes;
  bke::SpanAttributeWriter<float> plastic_writer;
  Vector<int> plastic_ids;
  Vector<float> plastic_before,plastic_after;
  const bool profile=profile_v26b_requested();
  const auto total_start=std::chrono::steady_clock::now();
  double prepare_time=0.0,contact_time=0.0,stage_time=0.0;
  double contact_scale=1.0;
  sculpt_physics::PreparedNativeBrushDab result;
  std::shared_ptr<NativePhysicsStroke> state;
  try {
    state=std::static_pointer_cast<NativePhysicsStroke>(cache.xpbd_v15_state);
    const Span<int2> edges=mesh.edges();
    auto &topology_cache=mesh.runtime->sculpt_physics_topology_cache;
    if (state && state->compact &&
        (!topology_cache.is_cached() || topology_cache.data().get()!=state->compact.get())) {
      throw std::logic_error("Physical brush connectivity cache invalidated mid-stroke");
    }
    if (state && (state->verts_num!=mesh.verts_num ||
                  state->edges_num!=mesh.edges_num ||
                  state->mesh_identity!=&mesh || state->edge_data!=edges.data())) {
      throw std::logic_error("V15 topology count changed during a native stroke");
    }

    // PBVH boundary nodes may repeat a global vertex. Prefer the largest
    // already-computed native translation; never apply it in multiple nodes.
    std::unordered_map<std::size_t,sculpt_physics::Vec3> dedup;
    dedup.reserve(native_translations.size());
    for (const auto &sample : native_translations) {
      if (sample.global_index>=std::size_t(mesh.verts_num) ||
          !std::isfinite(sample.translation.x) ||
          !std::isfinite(sample.translation.y) ||
          !std::isfinite(sample.translation.z)) {
        throw std::invalid_argument("V15 non-finite stock brush translation");
      }
      // Zero targets cannot win the existing largest-translation rule
      // and are discarded later anyway. Avoid inserting whole PBVH leaves.
      if(optimize_v27c_requested() &&
         std::hypot(sample.translation.x,sample.translation.y,sample.translation.z)<=1e-15)continue;
      const auto it=dedup.find(sample.global_index);
      if (it==dedup.end()) dedup.emplace(sample.global_index,sample.translation);
      else {
        const sculpt_physics::Vec3 prev=it->second;
        const double oldsize=std::hypot(prev.x,prev.y,prev.z);
        const auto &v=sample.translation;
        const double newsize=std::hypot(v.x,v.y,v.z);
        if (newsize>oldsize) it->second=v;
      }
    }
    std::vector<sculpt_physics::NativeBrushTranslation> samples;
    samples.reserve(dedup.size());
    for (const auto &item : dedup) {
      const auto &v=item.second;
      if (std::hypot(v.x,v.y,v.z)>1.0e-15) samples.push_back({item.first,v});
    }
    if (samples.empty()) {
      if(profile)std::fprintf(stderr,"[V27 no_write] stage=targets action=%d targets=0\n",cache.xpbd_v16_clay_behavior);
      return true;
    }
    const auto edge_at=[&](std::size_t id) {
      const int2 edge=edges[int(id)];
      return std::pair<std::size_t,std::size_t>{std::size_t(edge[0]),std::size_t(edge[1])};
    };
    const auto current_position=[&](std::size_t id)->sculpt_physics::Vec3 {
      if (id>=std::size_t(current.size())) throw std::out_of_range("V15 stale vertex ID");
      const float3 p=current[int(id)];
      return {double(p.x),double(p.y),double(p.z)};
    };
    const MeshAttributeData attributes(mesh);
    const auto inverse_mass=[&](std::size_t id) {
      const int i=int(id);
      const bool hidden=!attributes.hide_vert.is_empty() && attributes.hide_vert[i];
      const float mask=attributes.mask.is_empty() ? 0.0f : attributes.mask[i];
      return hidden || mask>=0.999f ? 0.0 : 1.0;
    };
    if (!state) {
      state=std::make_shared<NativePhysicsStroke>();
      state->verts_num=mesh.verts_num;
      state->edges_num=mesh.edges_num;
      state->mesh_identity=&mesh;
      state->edge_data=edges.data();
      if (cache.xpbd_v16_compact) {
        const bool warm=topology_cache.is_cached();
        topology_cache.ensure([&](std::shared_ptr<const void> &data) {
          data=std::make_shared<const sculpt_physics::CompactBrushTopology>(
              std::size_t(mesh.verts_num),std::size_t(mesh.edges_num),edge_at);
        });
        state->compact=std::static_pointer_cast<const sculpt_physics::CompactBrushTopology>(
            topology_cache.data());
        std::fprintf(stderr,"[V16 physics topology] %s\n",warm ? "REUSED" : "BUILT");
      }
      else {
        state->rest_world.reserve_topology(std::size_t(mesh.verts_num),
                                           std::size_t(mesh.edges_num));
        for (const int i : current.index_range()) {
          state->rest_world.add_particle(current_position(i),inverse_mass(i));
        }
        for (const int2 edge : edges) {
          try {
            state->rest_world.add_bond(std::size_t(edge[0]),std::size_t(edge[1]),1.0e-5);
          }
          catch (const std::invalid_argument &) {
            // Degenerate edges cannot define physical rest-length constraints.
          }
        }
        state->topology=std::make_unique<sculpt_physics::GpuTopologyBondIndex>(
            state->rest_world,1);
      }
    }
    sculpt_physics::NativeBrushDab dab;
    dab.kind=kind;
    static const bool colored=[]() {
      const char *raw=std::getenv("BLENDER_SCULPT_XPBD_V16_COLORED");
      return raw && raw[0]=='1' && raw[1]=='\0';
    }();
    dab.colored_order=colored;
    dab.dense_ids_v28=refine_v28b_requested() && optimize_v27c_requested();
    dab.assist=autonomous ? 1.0 : cache.xpbd_v16_assist;
    dab.clay_material=!autonomous && cache.xpbd_v16_clay_material;
    dab.clay_yield=cache.xpbd_v16_clay_yield;
    dab.clay_plasticity=cache.xpbd_v16_clay_plasticity;
    dab.clay_spread=cache.xpbd_v16_clay_spread;
    if (dab.clay_material &&
        (kind==sculpt_physics::NativeSculptBrushKind::Clay ||
         kind==sculpt_physics::NativeSculptBrushKind::ClayStrips)) {
      dab.clay_behavior=cache.xpbd_v16_clay_behavior;
      const float3 center=cache.location_symm;
      const float3 stroke_delta=cache.first_time ? float3(0.0f) :
                                  cache.location_symm-cache.last_location_symm;
      dab.clay_center={double(center.x),double(center.y),double(center.z)};
      dab.clay_radius=double(cache.radius);
      dab.clay_stroke_delta={double(stroke_delta.x),double(stroke_delta.y),double(stroke_delta.z)};
    }
    dab.compliance=1.0e-6;
    // Compare against the old V15 baseline before enabling new profiles.
    static const bool profiles=[]() {
      const char *raw=std::getenv("BLENDER_SCULPT_XPBD_V15_PROFILES");
      return raw && raw[0]=='1' && raw[1]=='\0';
    }();
    if (profiles || cache.xpbd_v16_clay_material) {
      const auto brush_profile=sculpt_physics::experimental_native_brush_profile(kind);
      dab.compliance=brush_profile.target_compliance;
      dab.material=brush_profile.material;
    }
    // Native XPBD Sculpt assets keep Blender's original Clay plane and
    // pressure/falloff. They never invoke an autonomous material action.
    const Brush &active_brush=*BKE_paint_brush_for_read(&sd.paint);
    const bool live_surface=live_surface_v30_brush(object,active_brush);
    const bool native_clay_assist=active_brush.sculpt_physics_mode==2 &&
        !autonomous && !dab.clay_material &&
        (kind==sculpt_physics::NativeSculptBrushKind::Clay ||
         kind==sculpt_physics::NativeSculptBrushKind::ClayStrips);
    if (native_clay_assist && (drag_contact_requested() || live_surface)) {
      const auto brush_profile=sculpt_physics::experimental_native_brush_profile(kind);
      dab.compliance=brush_profile.target_compliance;
      dab.material=brush_profile.material;
      dab.current_rest=true;
      if(stable_v28_requested() && refine_v28b_requested() && dab.assist>0.0){
        dab.post_stability_v28=true;
        dab.stability.relative_limit=material_override("BLENDER_SCULPT_V28_CLAY_EDGE_LIMIT",.35,.05,.8);
        dab.stability.relaxation=0.0;
        dab.stability.iterations=2;
        if(profile && !cache.xpbd_v15_active_logged)
          std::fprintf(stderr,"[V28B XPBD] kind=%d edge_limit=%.3f dense_ids=%d\n",int(kind),dab.stability.relative_limit,int(dab.dense_ids_v28));
      }
    }
    // Read the overrides once per process. Artist tuning preserves the
    // original brush pressure/falloff and changes only the physical solve.
    static const double bond_override=material_override(
        "BLENDER_SCULPT_XPBD_V16_BOND_SCALE",-1.0,0.0,1000.0);
    static const double follow_override=material_override(
        "BLENDER_SCULPT_XPBD_V16_REST_FOLLOW",-1.0,0.0,1.0);
    static const double compliance_override=material_override(
        "BLENDER_SCULPT_XPBD_V16_TARGET_COMPLIANCE",-1.0,0.0,1.0);
    if (bond_override>=0.0) dab.material.bond_compliance_scale=bond_override;
    if (follow_override>=0.0) dab.material.native_rest_follow=follow_override;
    if (compliance_override>=0.0) dab.compliance=compliance_override;
    if (autonomous && !conservative_clay) {
      // Independent intent is already the full material-driving target.
      // Never run V3 footprint redistribution a second time.
      // Current rest-follow is a preview of compliant surface response,
      // not persistent plastic strain. Retain a small elastic component.
      dab.material={100.0,0.95};
      dab.compliance=1.0e-8;
      // V18 is the default for autonomous actions, V17 remains available
      // as an A/B reference via a process switch, with no DNA/RNA rebuild.
      static const bool stable=[]() {
        const char *v=std::getenv("BLENDER_SCULPT_PHYSICS_V18");
        return !(v && v[0]=='0' && v[1]=='\0');
      }();
      dab.stability.enabled=stable;
      if (stable) {
        dab.stability.relative_limit=material_override(
            "BLENDER_SCULPT_V18_EDGE_LIMIT",0.20,0.02,0.8);
        dab.stability.relaxation=material_override(
            "BLENDER_SCULPT_V18_RELAXATION",0.15,0.0,0.5);
        dab.material={1.0,0.15};
        dab.compliance=1.0e-6;
        if (!cache.xpbd_v15_active_logged) {
          std::fprintf(stderr,"[V18 stability] ACTIVE edge_limit=%.3f relax=%.3f\n",
                       dab.stability.relative_limit,dab.stability.relaxation);
        }
      }
    }
    if (conservative_clay) {
      // The new solver already limits the entire patch with one common
      // scale. XPBD/relaxation would independently alter its flux budget.
      // Reuse the validated compact writer but stage exact target positions.
      dab.assist=0.0;
      dab.clay_material=false;
      dab.stability.enabled=false;
    }
    dab.topology_generation=1;
    std::vector<std::size_t> local_to_global;
    const auto solve_start=std::chrono::steady_clock::now();
    if (state->compact) {
      // The compact path stores connectivity only, and snapshots original
      // coordinates on first actual write. Untouched vertices retain their
      // stroke-start coordinates. No full-mesh physics World is allocated.
      auto prepared=sculpt_physics::prepare_compact_native_brush_dab(
          *state->compact,samples,edge_at,
          [&](std::size_t id) {
            if(live_surface)return current_position(id);
            const auto original=state->original_positions.find(id);
            return original==state->original_positions.end() ?
                       current_position(id) : original->second;
          },current_position,inverse_mass,dab,stroke_v27_requested() && conservative_clay,
          optimize_v27c_requested()?&state->region_cache:nullptr);
      if(profile && optimize_v27c_requested() && !conservative_clay){
        std::fprintf(stderr,"[V27C region] kind=%d hits=%zu misses=%zu active=%zu halo=%zu\n",
          int(kind),state->region_cache.hits,state->region_cache.misses,
          state->region_cache.active.size(),state->region_cache.region.vertices.size());
      }
      local_to_global=std::move(prepared.local_to_global);
      result=std::move(prepared.prepared);
    }
    else {
      result=sculpt_physics::prepare_cpu_native_brush_dab(
          state->rest_world,*state->topology,samples,current_position,dab);
    }

    const double prepare_ms=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-solve_start).count();

    prepare_time=prepare_ms;
    // True zero-impact dab: don't traverse, clip or rewrite every PBVH
    // node with zero translations. Blender's ordinary smoothing/plane
    // clipping may otherwise modify stationary coordinates unexpectedly.
    if (result.writes().empty()) {
      if(profile)std::fprintf(stderr,"[V27 no_write] stage=executor action=%d targets=%zu\n",cache.xpbd_v16_clay_behavior,samples.size());
      // A solver can also legitimately return an empty batch (e.g. all
      // motion below writeback precision). Acknowledge it without touching
      // native geometry or invalidating PBVH caches.
      result.finish_editor_success();
      cache.xpbd_v15_state=state;
      return true;
    }

    std::unordered_map<std::size_t,float3> changes;
    changes.reserve(result.writes().size());
    for (const auto &w : result.writes()) {
      const std::size_t global_id=state->compact ? local_to_global.at(w.global_index) :
                                                 w.global_index;
      const float3 p=current[int(global_id)];
      const float3 displacement{
          float(w.position.x-double(p.x)),
          float(w.position.y-double(p.y)),
          float(w.position.z-double(p.z))};
      if (!std::isfinite(displacement.x) ||
          !std::isfinite(displacement.y) ||
          !std::isfinite(displacement.z)) {
        throw std::runtime_error("V15 physics displacement out of float32 range");
      }
      changes.emplace(global_id,displacement);
    }

    const auto contact_start=std::chrono::steady_clock::now();
    if (drag_contact_requested() && state->compact &&
        kind!=sculpt_physics::NativeSculptBrushKind::Gravity) {
      const bool local_scrape=live_surface && (contact_v30b_requested() || adaptive_v31_requested()) && autonomous &&
                              cache.xpbd_v16_clay_behavior==14;
      const bool balanced_local=live_surface && (overlap_v30c_requested() || adaptive_v31_requested()) && conservative_clay &&
                                autonomous && cache.xpbd_v16_clay_behavior!=1 &&
                                cache.xpbd_v16_clay_behavior!=4 && cache.xpbd_v16_clay_behavior!=14;
      const bool coherent_v31b=autonomous && conservative_clay && footprint_v30e_brush(object,active_brush) &&
          ((adaptive_v31_requested() && direct_material_action_v31(cache.xpbd_v16_clay_behavior)) ||
           (continuous_contact_v31d2_requested() && material_override("BLENDER_SCULPT_V31D_AVERAGING",.65,0.0,1.0)>0 &&
            average_transport_action_v31d(cache.xpbd_v16_clay_behavior)));
      const auto result=limit_drag_contact(mesh,current,changes,conservative_clay && !local_scrape,balanced_local,coherent_v31b);
      contact_scale=result.final_scale;
      if (!cache.xpbd_v15_active_logged || result.final_scale<0.25) {
        std::fprintf(stderr,
            "[V19 contact] triangle_scale=%.5f final_scale=%.5f limited=%zu writes=%zu\n",
            result.minimum_scale,result.final_scale,result.limited_vertices,changes.size());
      }
    }

    if(global_collision_v29_requested()){
      const auto tris=mesh.corner_tris();
      const auto corners=mesh.corner_verts();
      const auto tri_at=[&](std::size_t i){
        const int3 tri=tris[int(i)];
        return std::array<std::size_t,3>{std::size_t(corners[tri[0]]),std::size_t(corners[tri[1]]),std::size_t(corners[tri[2]])};
      };
      const auto motion_at=[&](std::size_t i)->sculpt_physics::Vec3{
        const auto it=changes.find(i);
        if(it==changes.end())return {};
        return {double(it->second.x),double(it->second.y),double(it->second.z)};
      };
      const auto guarded=sculpt_physics::guard_global_collision_v29(
          std::size_t(tris.size()),tri_at,current_position,motion_at,
          std::max(1e-12,double(cache.radius)*1e-6));
      if(profile || guarded.scale<1)
        std::fprintf(stderr,"[V29 collision] scale=%.7g pairs=%zu limited=%zu unresolved=%zu budget=%d\n",
            guarded.scale,guarded.tested_pairs,guarded.limited_pairs,guarded.unresolved_pairs,int(guarded.budget_exhausted));
      contact_scale*=guarded.scale;
      if(guarded.scale==0){
        result.cancel();cache.xpbd_v15_state=state;return true;
      }
      if(guarded.scale<1)for(auto &item:changes)item.second*=float(guarded.scale);
    }
    contact_time=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-contact_start).count();
    const auto stage_start=std::chrono::steady_clock::now();
    if (!cache.xpbd_v15_active_logged) {
      double max_native=0.0,max_change=0.0,max_deviation=0.0;
      for (const auto &sample : samples) {
        if (inverse_mass(sample.global_index)<=0.0) continue;
        const auto &v=sample.translation;
        const auto it=changes.find(sample.global_index);
        const float3 t=it==changes.end() ? float3(0.0f) : it->second;
        max_native=std::max(max_native,std::hypot(v.x,v.y,v.z));
        max_change=std::max(max_change,std::hypot(double(t.x),double(t.y),double(t.z)));
        max_deviation=std::max(max_deviation,
            std::hypot(double(t.x)-v.x,double(t.y)-v.y,double(t.z)-v.z));
      }
      std::fprintf(stderr,
          "[%s physics trial] targets=%zu writes=%zu prepare_ms=%.3f target_max=%.6g physical_max=%.6g deviation_ratio=%.3f\n",
          conservative_clay ? "V19" : autonomous ? "V17" : "V16",samples.size(),changes.size(),prepare_ms,max_native,max_change,
          max_native>0.0 ? max_deviation/max_native : 0.0);
    }

    std::unordered_set<std::size_t> covered;
    covered.reserve(changes.size());
    node_mask.foreach_index([&](const int ni) {
      PreparedNode node;
      node.id=ni;
      const Span<int> verts=nodes[ni].verts();
      node.translations.resize(verts.size());
      for (const int i : verts.index_range()) {
        const std::size_t vi=std::size_t(verts[i]);
        const auto it=changes.find(vi);
        const bool first=it!=changes.end() && covered.insert(vi).second;
        node.translations[i]=first ? it->second : float3(0.0f);
      }
      const Vector<float3> before_clip=node.translations;
      clip_and_lock_translations(sd,ss,current,verts,node.translations);
      for (const int i : verts.index_range()) {
        const float3 wanted=before_clip[i];
        const float3 got=node.translations[i];
        // A zero translation must remain exactly zero, even if native
        // clipping would otherwise snap a stationary or pinned vertex.
        if (wanted.x==0.0f && wanted.y==0.0f && wanted.z==0.0f &&
            (got.x!=0.0f || got.y!=0.0f || got.z!=0.0f)) {
          throw std::runtime_error("V15 clipping changed stationary vertex");
        }
        const float tol=1e-6f*(1.0f+math::length(wanted));
        if (((conservative_clay || drag_contact_requested()) &&
             (got.x!=wanted.x || got.y!=wanted.y || got.z!=wanted.z)) ||
            !std::isfinite(got.x) || !std::isfinite(got.y) ||
            !std::isfinite(got.z) ||
            std::abs(got.x-wanted.x)>tol ||
            std::abs(got.y-wanted.y)>tol ||
            std::abs(got.z-wanted.z)>tol) {
          throw std::runtime_error("V15 locked/clipped mesh: refuse before write");
        }
      }
      editor_writes.push_back(std::move(node));
    });
    if (covered.size()!=changes.size()) {
      throw std::logic_error("V15 physics writes outside native PBVH Undo nodes");
    }
    if (state->compact) {
      // Record only vertices actually about to move, before any editor write.
      // This also covers tiny corrections produced by native clipping.
      for (const PreparedNode &node : editor_writes) {
        const Span<int> verts=nodes[node.id].verts();
        for (const int i : verts.index_range()) {
          const float3 t=node.translations[i];
          if (t.x!=0.0f || t.y!=0.0f || t.z!=0.0f) {
            const std::size_t id=std::size_t(verts[i]);
            state->original_positions.try_emplace(id,current_position(id));
          }
        }
      }
    }
    if(plastic_dab && plastic_dab->plastic_v29){
      // Attribute must already exist on the original mesh, created outside
      // Sculpt Mode. Never allocate an attribute in a position-only Undo step.
      Mesh &wmesh=*id_cast<Mesh *>(object.data);
      plastic_writer=wmesh.attributes_for_write().lookup_for_write_span<float>(".bse_plastic_compaction");
      if(!plastic_writer || plastic_writer.domain!=bke::AttrDomain::Point ||
         plastic_writer.span.size()!=mesh.verts_num)
        throw std::runtime_error("V29 compaction attribute unavailable");
      const auto ax=plastic_dab->axis;
      const double len=std::hypot(ax.x,ax.y,ax.z);
      if(len<=1e-12)throw std::runtime_error("Invalid V29 plastic axis");
      for(const auto &node:editor_writes){
        const Span<int> verts=nodes[node.id].verts();
        for(const int i:verts.index_range()){
          const auto &v=node.translations[i];
          const double normal=(ax.x*v.x+ax.y*v.y+ax.z*v.z)/len;
          const int id=verts[i];
          const float old=plastic_writer.span[id];
          const float next=float(sculpt_physics::update_compaction_v29(old,normal,plastic_dab->radius,plastic_dab->sign));
          if(next!=old){plastic_ids.append(id);plastic_before.append(old);plastic_after.append(next);}
        }
      }
      if(!plastic_ids.is_empty() &&
         !undo::capture_physics_compaction_v29(object,plastic_ids.as_span(),plastic_before.as_span()))
        throw std::runtime_error("V29 compaction requires native position Undo");
    }
    stage_time=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-stage_start).count();
  }
  catch (const std::exception &ex) {
    std::fprintf(stderr,"[V15 physical brush] disabled before write: %s\n",ex.what());
    if(plastic_writer)plastic_writer.finish();
    result.cancel();
    cache.xpbd_v15_state.reset();
    cache.xpbd_v15_failed=true;
    return conservative_clay || cache.xpbd_v15_active_logged;
  }

  // Full native Blender Undo journal/partial PBVH write rollback is not yet
  // implemented; this gated preview must not be enabled in release builds.
  const auto write_start=std::chrono::steady_clock::now();
  std::size_t moved_v30c=0,staged_v30c=0;
  for (PreparedNode &node : editor_writes) {
    if(profile && overlap_v30c_requested()){
      const Span<int> ids=nodes[node.id].verts();
      for(const int i:ids.index_range()){
        const float3 p=current[ids[i]],q=p+node.translations[i];
        ++staged_v30c;
        if(q.x!=p.x || q.y!=p.y || q.z!=p.z)++moved_v30c;
      }
    }
    bke::pbvh::MeshNode &pbvh_node=nodes[node.id];
    position_data.deform(node.translations,pbvh_node.verts());
    bke::pbvh::update_node_bounds_mesh(position_data.eval,pbvh_node);
  }
  if(plastic_writer){
    for(const int i:plastic_ids.index_range())plastic_writer.span[plastic_ids[i]]=plastic_after[i];
    plastic_writer.finish();
    if(profile)std::fprintf(stderr,"[V29 plastic] changed=%lld\n",static_cast<long long>(plastic_ids.size()));
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  result.finish_editor_success();
  cache.xpbd_v15_state=state;
  if(profile && overlap_v30c_requested())std::fprintf(stderr,"[V30C write] action=%d targets=%zu staged=%zu moved=%zu contact_scale=%.7g\n",cache.xpbd_v16_clay_behavior,native_translations.size(),staged_v30c,moved_v30c,contact_scale);
  if(profile){
    const auto now=std::chrono::steady_clock::now();
    std::fprintf(stderr,"[V26 profile] kind=%d action=%d targets=%zu prepare_ms=%.3f contact_ms=%.3f stage_ms=%.3f write_ms=%.3f total_ms=%.3f contact_scale=%.6f\n",
                 int(kind),cache.xpbd_v16_clay_behavior,native_translations.size(),prepare_time,contact_time,stage_time,
                 std::chrono::duration<double,std::milli>(now-write_start).count(),
                 std::chrono::duration<double,std::milli>(now-total_start).count(),contact_scale);
  }
  if (autonomous) {
    // Per-dab maxima accumulated only after the editor accepted the writes.
    // Sum of maxima is a motion-budget diagnostic, not net displacement.
    double peak=0.0;
    for (const PreparedNode &node : editor_writes) {
      for (const float3 &t : node.translations) {
        peak=std::max(peak,std::hypot(double(t.x),double(t.y),double(t.z)));
      }
    }
    ++state->autonomous_dabs;
    state->autonomous_peak_sum+=peak;
    state->autonomous_peak=std::max(state->autonomous_peak,peak);
    if (state->autonomous_dabs==1 || state->autonomous_dabs%32==0) {
      std::fprintf(stderr,
          "[V17 autonomous stroke] dabs=%zu peak_sum=%.6g peak=%.6g last_peak=%.6g\n",
          state->autonomous_dabs,state->autonomous_peak_sum,state->autonomous_peak,peak);
    }
  }
  if (!cache.xpbd_v15_active_logged) {
    std::fprintf(stderr,"[V16 physical brush] CPU XPBD %s %s preview ACTIVE\n",
                 state->compact ? "compact" : "reference",
                 conservative_clay ? "V19 Conservative Clay" : autonomous ? "V17 Autonomous" : cache.xpbd_v16_clay_material &&
                     (kind==sculpt_physics::NativeSculptBrushKind::Clay ||
                      kind==sculpt_physics::NativeSculptBrushKind::ClayStrips) ?
                     (cache.xpbd_v16_clay_behavior==1 ? "Clay V3 Deposit" :
                      cache.xpbd_v16_clay_behavior==2 ? "Clay V3 Compress" :
                      cache.xpbd_v16_clay_behavior==3 ? "Clay V3 Flow" : "Clay V2") : "assist");
    cache.xpbd_v15_active_logged=true;
  }
  return true;
}

// V17-B is an explicit Clay/Clay Strips behavior choice. Existing V2/V3
// modes retain their exact path, and no other brush is silently upgraded.
inline bool tool_v25_requested()
{
  static const bool enabled=[]() {
    const char *v=std::getenv("BLENDER_SCULPT_TOOL_V25");
    return v && v[0]=='1' && v[1]=='\0';
  }();
  return enabled;
}

inline bool shape_v24_requested()
{
  if (tool_v25_requested()) return true;
  static const bool enabled=[]() {
    const char *v=std::getenv("BLENDER_SCULPT_SHAPE_V24");
    return v && v[0]=='1' && v[1]=='\0';
  }();
  return enabled;
}

inline bool smooth_v23_requested()
{
  if (shape_v24_requested()) return true;
  static const bool enabled=[]() {
    const char *v=std::getenv("BLENDER_SCULPT_SMOOTH_V23");
    return v && v[0]=='1' && v[1]=='\0';
  }();
  return enabled;
}

inline bool new_material_action_v23(int action)
{
  return action>=11 && action<=14;
}

inline bool density_v22_requested()
{
  if (smooth_v23_requested()) return true;
  static const bool enabled=[]() {
    const char *v=std::getenv("BLENDER_SCULPT_DENSITY_V22");
    return v && v[0]=='1' && v[1]=='\0';
  }();
  return enabled;
}

inline bool material_v21_requested()
{
  if (density_v22_requested()) return true;
  static const bool enabled=[]() {
    const char *v=std::getenv("BLENDER_SCULPT_MATERIAL_V21");
    return v && v[0]=='1' && v[1]=='\0';
  }();
  return enabled;
}

inline bool surface_contact_v20_requested()
{
  if (material_v21_requested()) return true;
  static const bool enabled=[]() {
    const char *v=std::getenv("BLENDER_SCULPT_SURFACE_V20");
    return v && v[0]=='1' && v[1]=='\0';
  }();
  return enabled;
}

inline bool autonomous_requested(const StrokeCache &cache)
{
  return cache.xpbd_v16_clay_material &&
      ((cache.xpbd_v16_clay_behavior>=4 && cache.xpbd_v16_clay_behavior<=10) ||
       (cache.xpbd_v16_compact && new_material_action_v23(cache.xpbd_v16_clay_behavior)) ||
       (drag_contact_requested() && cache.xpbd_v16_clay_behavior==2) ||
       (surface_contact_v20_requested() && cache.xpbd_v16_compact && cache.xpbd_v16_clay_behavior>=1 &&
        cache.xpbd_v16_clay_behavior<=3));
}

// Explicit opt-in while tuning the new solver; no RNA/DNA change.
inline bool conservative_clay_requested()
{
  static const bool value=[]() {
    const char *v=std::getenv("BLENDER_SCULPT_CLAY_V19");
    return v && v[0]=='1' && v[1]=='\0';
  }();
  return value;
}

inline sculpt_physics::ClaySurfaceResult plan_conservative_clay(
    const Mesh &mesh, const Span<float3> positions,
    const std::vector<sculpt_physics::PhysicalBrushIntentSample> &samples,
    const sculpt_physics::ClaySurfaceDab &dab,
    std::vector<sculpt_physics::NativeBrushTranslation> &goals,
    SurfaceContactHistoryV28 *history=nullptr)
{
  using namespace sculpt_physics;
  const auto convert=[](const float3 &v)->Vec3 {
    return {double(v.x),double(v.y),double(v.z)};
  };
  const MeshAttributeData attributes(mesh);
  const auto plastic_attr=mesh.attributes().lookup<float>(".bse_plastic_compaction",bke::AttrDomain::Point);
  const auto vertex_faces=mesh.vert_to_face_map();
  const auto faces=mesh.faces();
  const auto corners=mesh.corner_verts();
  const auto tris=mesh.corner_tris();
  std::vector<ClaySurfaceVertex> vertices;
  std::vector<std::size_t> global;
  std::vector<ClaySurfaceTriangle> triangles;
  std::unordered_map<std::size_t,std::size_t> local;
  std::vector<int> face_ids;
  const auto add_vertex=[&](std::size_t id) {
    if (id>=std::size_t(positions.size())) throw std::out_of_range("Stale clay vertex");
    const auto found=local.find(id);
    if (found!=local.end()) return found->second;
    const auto i=vertices.size();
    local.emplace(id,i);
    global.push_back(id);
    vertices.push_back({convert(positions[int(id)]),0.0});
    return i;
  };
  for (const auto &s:samples) {
    if (!s.writable || s.envelope<=0.0) continue;
    if (s.global_index>=std::size_t(positions.size())) throw std::out_of_range("Stale clay sample");
    const int vi=int(s.global_index);
    // Match the compact writer's immovable set exactly, or its late
    // filtering would remove a donor/recipient and break conservation.
    if ((!attributes.hide_vert.is_empty() && attributes.hide_vert[vi]) ||
        (!attributes.mask.is_empty() && attributes.mask[vi]>=0.999f)) continue;
    const auto i=add_vertex(s.global_index);
    vertices[i].influence=s.envelope;
    if(dab.plastic_v29 && plastic_attr)vertices[i].compaction_v29=(*plastic_attr)[vi];
    if(history && dab.dose>0 && dab.intensity>0){
      // Capture only the first eligible geometry, never requested or
      // unaccepted motion. Reference does not reset when a dab is clipped.
      const auto it=history->try_emplace(s.global_index,SurfaceContactRefV28{
          vertices[i].position,dab.radius}).first;
      vertices[i].contact_reference=it->second.position;
      vertices[i].contact_radius=it->second.radius;
    }
    for (const int f:vertex_faces[int(s.global_index)]) face_ids.push_back(f);
  }
  std::sort(face_ids.begin(),face_ids.end());
  face_ids.erase(std::unique(face_ids.begin(),face_ids.end()),face_ids.end());
  for (const int f:face_ids) {
    for (const int t:bke::mesh::face_triangles_range(faces,f)) {
      const int3 tri=tris[t];
      triangles.push_back({add_vertex(std::size_t(corners[tri[0]])),
                           add_vertex(std::size_t(corners[tri[1]])),
                           add_vertex(std::size_t(corners[tri[2]]))});
    }
  }
  ClaySurfaceResult result;
  if (dab.extended_material) {
    const int substeps=int(material_override("BLENDER_SCULPT_V21_SUBSTEPS",4.0,1.0,16.0));
    // V26 avoids sorting diagnostic edges on unreported dabs.
    static thread_local unsigned int count=0;
    const bool report=count++%64==0;
    auto integrated=solve_material_transport_v21(vertices,triangles,dab,substeps,
                                                 !(dab.tool_v26 || dab.fast_v26b) || report);
    result=std::move(integrated.surface);
    if (report) {
      std::fprintf(stderr,"[%s material] substeps=%d refinement_edges=%zu active=%zu proxy_nodes=%zu\n",
                   dab.tool_v25?"V25":dab.shape_transport?"V24":dab.smooth_proxy?"V23":dab.density_independent?"V22":"V21",integrated.substeps,
                   integrated.refinement_edges,result.active,result.transport_nodes);
    }
  }
  else result=solve_clay_surface(vertices,triangles,dab);
  goals.reserve(vertices.size());
  for (std::size_t i=0;i<vertices.size();++i) {
    const auto &v=result.displacement[i];
    if (std::hypot(v.x,v.y,v.z)>1e-15) goals.push_back({global[i],v});
  }
  return result;
}

inline bool try_apply_intent(
    const Sculpt &sd, Object &object, const IndexMask &node_mask,
    const PositionDeformData &position_data,
    const sculpt_physics::NativeSculptBrushKind kind,
    std::vector<sculpt_physics::PhysicalBrushIntentSample> &samples,
    const float3 &normal)
{
  StrokeCache &cache=*object.runtime->sculpt_session->cache;
  const bool v23=smooth_v23_requested() || new_material_action_v23(cache.xpbd_v16_clay_behavior);
  const bool surface_v20=(surface_contact_v20_requested() || v23) && cache.xpbd_v16_compact &&
      cache.xpbd_v16_clay_material && cache.xpbd_v16_clay_behavior>=1 &&
      cache.xpbd_v16_clay_behavior<=14;
  const bool conservative=surface_v20 || (conservative_clay_requested() &&
      kind==sculpt_physics::NativeSculptBrushKind::Clay &&
      (cache.xpbd_v16_clay_behavior==4 || cache.xpbd_v16_clay_behavior==6 ||
       cache.xpbd_v16_clay_behavior==9 || cache.xpbd_v16_clay_behavior==10) &&
      cache.xpbd_v16_compact);
  if (cache.xpbd_v15_failed) return conservative || cache.xpbd_v15_active_logged;
  try {
    // PBVH boundary duplicates must not double the impulse.
    std::sort(samples.begin(),samples.end(),[](const auto &a,const auto &b) {
      if (a.global_index!=b.global_index) return a.global_index<b.global_index;
      return a.envelope>b.envelope;
    });
    samples.erase(std::unique(samples.begin(),samples.end(),[](const auto &a,const auto &b) {
      return a.global_index==b.global_index;
    }),samples.end());
    const float3 center=cache.location_symm;
    const float3 delta=cache.xpbd_v17_intent_started ?
        center-cache.xpbd_v17_last_center : float3(0.0f);
    sculpt_physics::PhysicalBrushIntentDab intent;
    const auto convert=[](const float3 &v)->sculpt_physics::Vec3 {
      return {double(v.x),double(v.y),double(v.z)};
    };
    intent.center=convert(center);
    const double sign=std::signbit(cache.bstrength) ? -1.0 : 1.0;
    intent.normal=convert(normal)*sign;
    intent.stroke_direction=convert(delta)*sign;
    intent.radius=double(cache.radius);
    // Native bstrength includes brush-specific multipliers and overlap
    // attenuation. Do NOT use its magnitude on top of arc-length dose.
    // Reuse the actual tablet strength curve once, with linear artist gain.
    const Brush &brush=*BKE_paint_brush_for_read(&sd.paint);
    const double strength=double(BKE_brush_alpha_get(&sd.paint,&brush));
    const double pressure=BKE_brush_use_alpha_pressure(&brush) ?
        double(BKE_curvemapping_evaluateF(brush.curve_strength,0,cache.pressure)) : 1.0;
    if (!std::isfinite(strength) || !std::isfinite(pressure)) {
      throw std::invalid_argument("Invalid autonomous strength/pressure");
    }
    const bool v24=(shape_v24_requested() || cache.xpbd_v16_clay_behavior==14) &&
        cache.xpbd_v16_clay_behavior!=1 && cache.xpbd_v16_clay_behavior!=4;
    intent.intensity=std::clamp(strength,0.0,v24?4.0:1.0)*std::clamp(pressure,0.0,1.0)*
                     cache.xpbd_v16_assist;
    intent.dose=cache.xpbd_v17_intent_started ?
        std::clamp(double(math::length(delta))/std::max(intent.radius,1e-12),0.0,1.0) : 0.15;
    const int original_action=cache.xpbd_v16_clay_behavior;
    const bool v27=stroke_v27_requested() && conservative;
    std::shared_ptr<sculpt_physics::StrokeContextV27> stroke;
    sculpt_physics::StrokeStepV27 stroke_step;
    if(v27){
      stroke=std::static_pointer_cast<sculpt_physics::StrokeContextV27>(cache.xpbd_v27_stroke);
      if(!stroke)stroke=std::make_shared<sculpt_physics::StrokeContextV27>();
      stroke_step=sculpt_physics::stage_stroke_v27(*stroke,convert(center),convert(normal),
                                                  intent.radius,pressure);
      // Deposit remains a behavior-control reference; only direct writes
      // are optimized. Other actions use radius-averaged segment dose.
      if(original_action!=1 && original_action!=4)intent.dose=stroke_step.dose;
    }
    const int action=surface_v20 && original_action>=1 && original_action<=3 ?
                         original_action+3 :
                     drag_contact_requested() && original_action==2 ? 5 : original_action;
    if ((action>=7 || conservative || drag_contact_requested()) && brush.stroke_method==BRUSH_STROKE_AIRBRUSH &&
        cache.xpbd_v17_intent_started) {
      // Native timer dispatch already provides repeated stationary dabs.
      // Use nominal timer seconds, not wall-clock/frame time or cursor
      // jitter. No timer is created by this bridge.
      const double rate=double(brush.rate);
      if (!std::isfinite(rate) || rate<=0.0) {
        throw std::invalid_argument("Invalid physical airbrush rate");
      }
      const double dwell=std::clamp(rate*1.5,0.0,1.0);
      intent.dose=v24?std::max(intent.dose,dwell):dwell;
    }
    intent.radial_sign=sign;
    intent.spread=cache.xpbd_v16_clay_spread;
    intent.limit_to_projected_disk=false; // factors already encode native brush shape
    using Action=sculpt_physics::PhysicalBrushAction;
    switch (action) {
      case 4: intent.action=Action::Deposit; break;
      case 5: intent.action=Action::Compress; break;
      case 6: intent.action=Action::Flow; break;
      case 7: intent.action=Action::Compact; break;
      case 8: intent.action=Action::Press; break;
      case 9: intent.action=Action::PressureFlow; break;
      case 10: intent.action=Action::HybridFlow; break;
      case 11: case 12: case 13: case 14:
        if (!surface_v20) throw std::invalid_argument("V23 material action requires compact mesh");
        break; // native planner bypassed by the surface transport path
      default: throw std::invalid_argument("Unknown autonomous action");
    }
    std::vector<sculpt_physics::NativeBrushTranslation> goals;
    double pending_press_dose=cache.xpbd_v26_press_dose;
    sculpt_physics::ClaySurfaceDab plastic_dab;
    if (conservative) {
      using namespace sculpt_physics;
      ClaySurfaceDab clay;
      clay.center=intent.center;
      clay.axis=v27 && v24 ? stroke_step.next.axis : convert(normal);
      clay.direction=convert(delta);
      clay.radius=intent.radius;
      clay.intensity=intent.intensity;
      clay.dose=intent.dose;
      clay.mobility=intent.spread;
      clay.sign=sign;
      clay.fast_v26b=fast_v26b_requested();
      clay.contact_v27b=v27 && contact_v27b_requested();
      clay.stable_v28=stable_v28_requested() && v27;
      clay.refine_v28b=clay.stable_v28 && refine_v28b_requested();
      clay.live_surface_v30=live_surface_v30_brush(object,brush);
      if(clay.live_surface_v30){
        // Refresh the geometric frame from the current sampled surface; the
        // stroke retains travel and pressure, not the previous tool plane.
        clay.axis=convert(normal);
        if(v27)stroke_step.next.axis=clay.axis/stroke_norm_v27(clay.axis);
      }
      clay.overlap_v30c=clay.live_surface_v30 && clay.stable_v28 && overlap_v30c_requested();
      clay.adaptive_v31=footprint_v30e_brush(object,brush) && adaptive_v31_requested() &&
                        (action==5 || action==7 || action==8 || action==14);
      clay.gradient_v31c=clay.adaptive_v31 && gradient_v31c_requested();
      if(average_surface_v31d_requested() && footprint_v30e_brush(object,brush) &&
         (clay.adaptive_v31 || average_transport_action_v31d(action)))
        clay.average_surface_v31d=material_override("BLENDER_SCULPT_V31D_AVERAGING",.65,0.0,1.0);
      clay.continuous_contact_v31d2=clay.average_surface_v31d>0 && continuous_contact_v31d2_requested();
      if(clay.continuous_contact_v31d2)clay.gradient_v31c=gradient_v31c_requested();
      if(clay.continuous_contact_v31d2 && motion_proxy_v31d3_requested())
        clay.motion_proxy_v31d3=material_override("BLENDER_SCULPT_V31D3_PROXY_BLEND",.65,0.0,1.0);
      clay.melt_fallback_v31c=clay.live_surface_v30 && clay.refine_v28b && action==11 && melt_fallback_v31c_requested();
      clay.contact_v30b=clay.live_surface_v30 && clay.refine_v28b && contact_v30b_requested() &&
                         (action==5 || action==7 || action==8 || action==14);
      clay.transport_v30b=clay.live_surface_v30 && clay.refine_v28b && transport_v30b_requested() &&
                          (action==6 || action==12 || action==13);
      clay.push_v30d=clay.transport_v30b && action==6 && push_v30d_requested();
      if(clay.push_v30d)clay.push_gain_v30d=material_override("BLENDER_SCULPT_V30D_PUSH_GAIN",2.5,.5,4.0);
      if(clay.transport_v30b && v27){
        stage_transport_v30b(stroke_step,*stroke);
        if(action==6 || action==13){
          clay.direction=stroke_step.next.transport_direction_v30b;
          clay.transport_load_v30b=.55+.45*stroke_step.next.transport_load_v30b;
        }
      }
      clay.swept_v29=clay.refine_v28b && swept_v29_active(cache,brush);
      if(clay.swept_v29)clay.sweep_start=convert(cache.xpbd_v17_last_center);
      if(clay.contact_v27b)clay.melt_local_gravity_v27b=material_override("BLENDER_SCULPT_V27B_MELT_LOCAL_GRAVITY",0.0,0.0,1.0);
      if (surface_v20) {
        clay.surface_contact=true;
        clay.flow_fusion=original_action==3;
        clay.yield_slope=0.05;
        if (material_v21_requested() || v23) {
          clay.extended_material=true;
          clay.density_independent=density_v22_requested() || v23;
          clay.smooth_proxy=v23;
          clay.shape_transport=v24;
          clay.tool_v25=v24 && (tool_v25_requested() || original_action==14);
          const char *v26=std::getenv("BLENDER_SCULPT_TOOL_V26");
          clay.tool_v26=clay.tool_v25 && v26 && v26[0]=='1' && v26[1]=='\0';
          clay.continuous_v26b=clay.tool_v26 && clay.fast_v26b;
          if (clay.tool_v25) {
            clay.fold_gain=material_override("BLENDER_SCULPT_V25_FOLD",.22,0.0,1.0);
            clay.detail_retention=material_override("BLENDER_SCULPT_V25_DETAIL",.65,0.0,1.0);
            clay.tool_depth=material_override("BLENDER_SCULPT_V25_DEPTH",.07,0.0,.25);
          }
          if (v23) clay.proxy_cells_per_radius=int(material_override(
              "BLENDER_SCULPT_V23_CELLS",8.0,4.0,16.0));
          // Preserve existing preset/RNA controls; remaining settings are
          // process-local tuning knobs until material UI is validated.
          clay.plasticity=cache.xpbd_v16_clay_plasticity;
          clay.friction=material_override("BLENDER_SCULPT_V21_FRICTION",0.85,0.0,1.0);
          clay.viscosity=material_override("BLENDER_SCULPT_V21_VISCOSITY",0.2,0.0,1.0);
          clay.cohesion=material_override("BLENDER_SCULPT_V21_COHESION",0.35,0.0,1.0);
          clay.stiffness=material_override("BLENDER_SCULPT_V21_STIFFNESS",0.25,0.0,1.0);
          clay.volume_preservation=material_override("BLENDER_SCULPT_V21_VOLUME",0.75,0.0,1.0);
          clay.fusion=material_override("BLENDER_SCULPT_V21_FUSION",0.6,0.0,1.0);
          if (clay.tool_v25) {
            const char *material=std::getenv("BLENDER_SCULPT_V25_MATERIAL");
            if (material && material[0]) {
              const auto p=surface_profile_v25(material);
              clay.friction=material_override("BLENDER_SCULPT_V21_FRICTION",p.friction,0.0,1.0);
              clay.viscosity=material_override("BLENDER_SCULPT_V21_VISCOSITY",p.viscosity,0.0,1.0);
              clay.cohesion=material_override("BLENDER_SCULPT_V21_COHESION",p.cohesion,0.0,1.0);
              clay.stiffness=material_override("BLENDER_SCULPT_V21_STIFFNESS",p.stiffness,0.0,1.0);
              clay.plasticity=material_override("BLENDER_SCULPT_V25_PLASTICITY",p.plasticity,0.0,1.0);
              clay.volume_preservation=material_override("BLENDER_SCULPT_V21_VOLUME",p.volume_preservation,0.0,1.0);
              clay.fusion=material_override("BLENDER_SCULPT_V21_FUSION",p.fusion,0.0,1.0);
              clay.detail_retention=material_override("BLENDER_SCULPT_V25_DETAIL",p.detail_retention,0.0,1.0);
            }
          }
        }
        switch(action) {
          case 4: clay.action=ClaySurfaceAction::FusionDeposit; break;
          case 5: clay.action=ClaySurfaceAction::Compress; break;
          case 6: clay.action=ClaySurfaceAction::Push; break;
          case 7: clay.action=ClaySurfaceAction::Compact; break;
          case 8: clay.action=ClaySurfaceAction::Press; break;
          case 9: clay.action=ClaySurfaceAction::PressureFlow; break;
          case 10: clay.action=ClaySurfaceAction::HybridFlow; break;
          case 11: clay.action=ClaySurfaceAction::Melt; break;
          case 12: clay.action=ClaySurfaceAction::Gather; break;
          case 13: clay.action=ClaySurfaceAction::Smear; break;
          case 14: clay.action=ClaySurfaceAction::Scrape; break;
          default: throw std::invalid_argument("Unknown V20 material action");
        }
      }
      else if (action==4) clay.action=ClaySurfaceAction::Buildup;
      else if (action==9) {
        clay.action=sign<0 ? ClaySurfaceAction::Gather : ClaySurfaceAction::Spread;
        clay.sign=1.0;
      }
      else if (action==10 && math::length(delta)<=1e-8f) {
        clay.action=ClaySurfaceAction::Gather;
      }
      else clay.action=ClaySurfaceAction::Push;
      if (clay.tool_v25 && (action==8 || action==14) &&
          material_override("BLENDER_SCULPT_V25_LOCK_PLANE",1.0,0.0,1.0)>0.5) {
        if(clay.live_surface_v30){
          // Cursor ray hits the accepted mesh. Apply an incremental cut/press
          // relative to this contact instead of chasing a previous track.
          clay.has_tool_plane=true;
          clay.tool_plane_height=-sign*clay.tool_depth*clay.radius;
          pending_press_dose=0.0;
          if(v27){stroke_step.next.tool_reference=clay.center;stroke_step.next.tool_plane_valid=true;}
          if(profile_v26b_requested())std::fprintf(stderr,"[V30 contact] action=%d fresh_plane=%.7g\n",action,clay.tool_plane_height);
        }else if(clay.contact_v27b){
          const bool reset=!stroke->tool_plane_valid || stroke_step.gap ||
              stroke_dot_v27(stroke_step.next.axis,stroke->axis)<.85;
          const bool supported=track_tool_plane_v27(stroke_step,*stroke,samples,clay.refine_v28b);
          if(reset)pending_press_dose=0;
          double depth=clay.tool_depth;
          if(action==8 && clay.tool_v26 && !clay.refine_v28b){
            pending_press_dose=std::min(1.0,pending_press_dose+clay.dose*clay.intensity);
            depth=progressive_press_depth_v26(depth,pending_press_dose);
          }
          // V28B: fixed artist depth, with geometry-local saturation.
          // Do not deepen a newly encountered area just because the stroke is long.
          clay.axis=stroke_step.next.axis;
          clay.has_tool_plane=true;
          clay.tool_plane_height=stroke_dot_v27(stroke_step.next.tool_reference-clay.center,clay.axis)-
                                sign*depth*clay.radius;
          if(profile_v26b_requested())std::fprintf(stderr,
              "[V27B contact] action=%d plane=%.7g depth=%.7g reset=%d support=%d\n",
              action,clay.tool_plane_height,depth,int(reset),int(supported));
        }else{
        const float3 axis=v27 ? float3(float(clay.axis.x),float(clay.axis.y),float(clay.axis.z)) : math::normalize(normal);
        if (!cache.xpbd_v25_plane_valid) {
          pending_press_dose=0.0;
          cache.xpbd_v25_plane_origin=center;
          cache.xpbd_v25_plane_normal=axis;
          cache.xpbd_v25_plane_valid=true;
        }
        const double alignment=double(math::dot(axis,cache.xpbd_v25_plane_normal));
        const double height=double(math::dot(cache.xpbd_v25_plane_origin-center,
                                             cache.xpbd_v25_plane_normal));
        // A finite local plane, not a globally locked plane across a
        // highly curved object. Re-anchor when leaving its valid chart.
        const bool reanchor=alignment<0.85 || std::abs(height)>clay.radius*.30;
        if (reanchor) {
          pending_press_dose=0.0;
          cache.xpbd_v25_plane_origin=center;
          cache.xpbd_v25_plane_normal=axis;
        }
        if (!reanchor || clay.tool_v26) {
          double depth=clay.tool_depth;
          if (clay.tool_v26 && action==8) {
            pending_press_dose=std::min(1.0,pending_press_dose+clay.dose*clay.intensity);
            depth=progressive_press_depth_v26(depth,pending_press_dose);
          }
          clay.axis=convert(cache.xpbd_v25_plane_normal);
          clay.has_tool_plane=true;
          clay.tool_plane_height=(reanchor?0.0:height)-sign*depth*clay.radius;
        }
        }
      }
      const Mesh &mesh=*id_cast<Mesh *>(object.data);
      if (position_data.eval.size()!=mesh.verts_num) return false;
      clay.plastic_v29=clay.stable_v28 && plastic_v29_requested() &&
          (action==5 || action==7 || action==8) &&
          bool(mesh.attributes().lookup<float>(".bse_plastic_compaction",bke::AttrDomain::Point));
      if(clay.plastic_v29)plastic_dab=clay;
      const auto plan_start=std::chrono::steady_clock::now();
      std::shared_ptr<SurfaceContactHistoryV28> history;
      if(clay.stable_v28 && !clay.live_surface_v30 && clay.action!=ClaySurfaceAction::Buildup &&
         clay.action!=ClaySurfaceAction::FusionDeposit){
        history=std::static_pointer_cast<SurfaceContactHistoryV28>(cache.xpbd_v28_surface_history);
        if(!history){
          history=std::make_shared<SurfaceContactHistoryV28>();
          cache.xpbd_v28_surface_history=history;
        }
      }
      if(footprint_v30e_brush(object,brush) && footprint_audit_v30e_requested()){
        // Expensive independent reference, never changes the write mask.
        // Count geometrically eligible original mesh vertices absent from
        // gathered unique-node samples, before optional frontface/alpha.
        const MeshAttributeData attrs(mesh);
        const float3 axis=math::normalize(normal);
        // Adaptive path no longer applies a fixed axial depth; this audit
        // measures the enclosing spatial candidate bound, not geodesic reach.
        const double depth=clay.contact_v30b?.65:.50;
        const double spatial_bound=1.51*cache.radius+(clay.swept_v29?math::length(delta):0.0);
        std::size_t expected=0,missing=0,zero_factor=0,cursor=0;
        for(const int i:position_data.eval.index_range()){
          while(cursor<samples.size() && samples[cursor].global_index<std::size_t(i))++cursor;
          if((!attrs.hide_vert.is_empty() && attrs.hide_vert[i]) ||
             (!attrs.mask.is_empty() && attrs.mask[i]>=.999f))continue;
          const auto p=position_data.eval[i];
          if(((clay.adaptive_v31 || clay.average_surface_v31d>0)?math::length(p-center)>=spatial_bound:
              std::abs(math::dot(p-center,axis))>=depth*cache.radius) ||
             footprint_distance_v30e(cache,brush,p,normal)>=cache.radius)continue;
          ++expected;
          if(cursor==samples.size() || samples[cursor].global_index!=std::size_t(i))++missing;
          else if(!samples[cursor].writable || samples[cursor].envelope<=0)++zero_factor;
        }
        std::fprintf(stderr,"[V30E audit] action=%d expected=%zu missing_pbvh=%zu zero_factor=%zu samples=%zu\n",
                     action,expected,missing,zero_factor,samples.size());
      }
      auto planned=plan_conservative_clay(mesh,position_data.eval,samples,clay,goals,history.get());
      if(clay.motion_proxy_v31d3>0 && profile_v26b_requested())
        std::fprintf(stderr,"[V31D3 proxy] action=%d blend=%.3f passes=%zu rejected=%zu nodes=%zu strain_before=%.7g strain_after=%.7g budget_error=%.7g\n",action,clay.motion_proxy_v31d3,planned.proxy_passes_v31d3,planned.proxy_rejected_v31d3,planned.proxy_nodes_v31d3,planned.proxy_strain_before_v31d3,planned.proxy_strain_after_v31d3,planned.proxy_budget_error_v31d3);
      if(clay.continuous_contact_v31d2 && profile_v26b_requested())
        std::fprintf(stderr,"[V31D2 contact] action=%d mixed_support=%zu chart_rejected=%zu disconnected=%zu active=%zu goals=%zu scale=%.7g\n",action,planned.chart_recovered_v30b,planned.chart_rejected_v30b,planned.component_rejected_v30b,planned.active,goals.size(),planned.accepted_scale);
      if(clay.average_surface_v31d>0 && profile_v26b_requested())
        std::fprintf(stderr,"[V31D average] action=%d blend=%.3f fits=%zu recovered_pre_split=%zu peak_residual_R=%.7g active=%zu goals=%zu\n",action,clay.average_surface_v31d,planned.average_fits_v31d,planned.average_recovered_v31d,planned.average_peak_v31d,planned.active,goals.size());
      if(clay.adaptive_v31 && profile_v26b_requested())
        std::fprintf(stderr,"[V31 contact] action=%d candidates=%zu reached=%zu beyond_old_depth=%zu goals=%zu local_limits=%zu min_scale=%.7g signed_first_order=%.9g\n",action,planned.adaptive_candidates_v31,planned.adaptive_reached_v31,planned.adaptive_deep_v31,goals.size(),planned.local_limited_v30b,planned.accepted_scale,planned.signed_swept_volume);
      if(clay.overlap_v30c && profile_v26b_requested()){
        std::size_t eligible=0;
        for(const auto &sample:samples)if(sample.writable && sample.envelope>0)++eligible;
        std::fprintf(stderr,"[V30C coverage] action=%d candidates=%zu eligible=%zu active=%zu goals=%zu patches=%zu chart_rejected=%zu depth_rejected=%zu other_component=%zu patch_budget=%zu scale=%.7g\n",action,samples.size(),eligible,planned.active,goals.size(),planned.patches_v30c,planned.chart_rejected_v30b,planned.depth_rejected_v30b,planned.component_rejected_v30b,planned.patch_budget_skipped_v30c,planned.accepted_scale);
      }
      if(clay.live_surface_v30 && profile_v26b_requested())std::fprintf(stderr,"[V30 live] action=%d candidates=%zu active=%zu goals=%zu scale=%.7g history_limited=%zu\n",action,samples.size(),planned.active,goals.size(),planned.accepted_scale,planned.history_limited);
      if(clay.live_surface_v30 && clay.refine_v28b && action==11 && profile_v26b_requested())
        std::fprintf(stderr,"[V30B melt] fit_failures=%zu residual_peak_R=%.7g active=%zu proxy_nodes=%zu volume=%.8g scale=%.7g\n",planned.melt_fit_failures_v30b,planned.melt_residual_peak_v30b,planned.active,planned.transport_nodes,planned.transported_volume,planned.accepted_scale);
      if(clay.melt_fallback_v31c && profile_v26b_requested())
        std::fprintf(stderr,"[V31C melt] quadratic_failures=%zu affine_fallbacks=%zu active=%zu\n",planned.melt_fit_failures_v30b,planned.melt_affine_fallbacks_v31c,planned.active);
      if(clay.contact_v30b && profile_v26b_requested())std::fprintf(stderr,"[V30B contact] action=%d chart_rejected=%zu recovered=%zu depth_rejected=%zu component_rejected=%zu local_limits=%zu goals=%zu\n",action,planned.chart_rejected_v30b,planned.chart_recovered_v30b,planned.depth_rejected_v30b,planned.component_rejected_v30b,planned.local_limited_v30b,goals.size());
      if(clay.push_v30d && profile_v26b_requested())
        std::fprintf(stderr,"[V30D push] gain=%.3f load=%.5f dose=%.6f active=%zu goals=%zu volume=%.8g fine=%.8g scale=%.7g\n",clay.push_gain_v30d,clay.transport_load_v30b,clay.dose,planned.active,goals.size(),planned.transported_volume,planned.fine_transport_v30b,planned.accepted_scale);
      if(clay.transport_v30b && profile_v26b_requested())std::fprintf(stderr,"[V30B transport] action=%d load=%.6f fine_volume=%.8g volume=%.8g nodes=%zu\n",action,clay.transport_load_v30b,planned.fine_transport_v30b,planned.transported_volume,planned.transport_nodes);
      if(clay.swept_v29 && profile_v26b_requested())std::fprintf(stderr,"[V29 sweep] action=%d length=%.7g active=%zu\n",action,double(math::length(delta)),planned.active);
      if(clay.stable_v28 && profile_v26b_requested())
        std::fprintf(stderr,"[V28 stability] action=%d history_limited=%zu slope_scale=%.6f history_size=%zu transport_nodes=%zu\n",
                     action,planned.history_limited,planned.slope_scale,history?history->size():0,planned.transport_nodes);
      const double plan_ms=std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-plan_start).count();
      if(v27 && profile_v26b_requested())std::fprintf(stderr,"[V27 stroke] action=%d samples=%zu path=%.8g dose=%.6f gap=%d goals=%zu\n",action,stroke_step.next.samples,stroke_step.next.path_length,clay.dose,int(stroke_step.gap),goals.size());
      if(profile_v26b_requested())std::fprintf(stderr,"[V26 plan] action=%d active=%zu plan_ms=%.3f dose=%.5f scale=%.6f traces=%zu rejected=%zu\n",int(clay.action),planned.active,plan_ms,clay.dose,planned.accepted_scale,planned.trace_attempts,planned.trace_rejected);
      if (clay.shape_transport && (!cache.xpbd_v15_active_logged || planned.active==0)) {
        std::fprintf(stderr,"[V24 gesture] strength=%.3f dose=%.4f trace_attempts=%zu trace_rejected=%zu\n",
                     clay.intensity,clay.dose,planned.trace_attempts,planned.trace_rejected);
      }
      if (!cache.xpbd_v15_active_logged || (clay.shape_transport && planned.active==0)) {
        std::fprintf(stderr,"[%s clay surface] action=%d active=%zu target_volume=%.9g transport=%.9g scale=%.4f plan_ms=%.3f\n",
                     clay.tool_v25 ? "V25" : clay.shape_transport ? "V24" : clay.smooth_proxy ? "V23" : clay.density_independent ? "V22" : material_v21_requested() && surface_v20 ? "V21" : surface_v20 ? "V20" : "V19",int(clay.action),planned.active,planned.signed_swept_volume,
                     planned.transported_volume,planned.accepted_scale,plan_ms);
      }
    }
    else if (drag_contact_requested()) {
      using namespace sculpt_physics;
      auto history=std::static_pointer_cast<BrushContactHistory>(cache.xpbd_v19_contact_history);
      if (!history) {
        history=std::make_shared<BrushContactHistory>();
        cache.xpbd_v19_contact_history=history;
      }
      // Capture first eligible contact before geometry changes. No counters
      // advance on rejected dabs; compression is measured from actual mesh.
      const double len=std::hypot(intent.normal.x,intent.normal.y,intent.normal.z);
      if (!std::isfinite(len) || len<=1e-12) throw std::invalid_argument("Invalid contact axis");
      if (intent.intensity>0 && intent.dose>0 &&
          (action==5 || action==7 || action==8)) {
        const auto axis=intent.normal/len;
        for (const auto &v:samples) {
          if(v.writable && v.envelope>0)
            history->try_emplace(v.global_index,BrushContactReference{v.position,axis});
        }
      }
      goals=plan_drag_contact(samples,intent,*history);
    }
    else goals=sculpt_physics::plan_physical_brush_intent(samples,intent);
    const bool handled=try_apply(sd,object,node_mask,position_data,kind,goals,true,conservative,
                                 plastic_dab.plastic_v29?&plastic_dab:nullptr);
    if (handled && !cache.xpbd_v15_failed) {
      cache.xpbd_v17_intent_started=true;
      cache.xpbd_v17_last_center=center;
      cache.xpbd_v26_press_dose=pending_press_dose;
      if(v27){*stroke=stroke_step.next;cache.xpbd_v27_stroke=stroke;}
    }
    return handled;
  }
  catch (const std::exception &ex) {
    std::fprintf(stderr,"[V17/V19 intent] rejected before write: %s\n",ex.what());
    cache.xpbd_v15_failed=true;
    return conservative || cache.xpbd_v15_active_logged;
  }
}

inline void append_intent_samples(
    std::vector<sculpt_physics::PhysicalBrushIntentSample> &samples,
    const Span<int> verts,const Span<float3> positions,const Span<float> factors,
    const Span<float3> normals)
{
  for (const int j : verts.index_range()) {
    const int id=verts[j];
    const float3 p=positions[id];
    const double f=double(factors[j]);
    // Negative alpha is not supported by this first autonomous preview.
    // Do not turn invalid factors into unmasked targets.
    if (!std::isfinite(f)) continue; // fail closed for this sample
    const float3 n=normals[id];
    samples.push_back({std::size_t(id),{double(p.x),double(p.y),double(p.z)},
                       std::clamp(f,0.0,1.0),1.0,f>0.0,
                       {double(n.x),double(n.y),double(n.z)}});
  }
}

}  // namespace blender::ed::sculpt_paint::brushes::physics_v15
#endif
