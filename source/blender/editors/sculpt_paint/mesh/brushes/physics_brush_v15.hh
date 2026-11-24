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

#  include "BKE_mesh.hh"
#  include "BKE_multires_sculpt_layers.hh"
#  include "BKE_object_types.hh"
#  include "BKE_paint_bvh.hh"
#  include "DNA_scene_types.h"
#  include "BLI_math_vector.hh"

#  include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#  include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#  include <algorithm>
#  include <cmath>
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
  std::unique_ptr<sculpt_physics::CompactBrushTopology> compact;
  std::unordered_map<std::size_t, sculpt_physics::Vec3> original_positions;
  const Mesh *mesh_identity = nullptr;
  const int2 *edge_data = nullptr;
  int verts_num = 0;
  int edges_num = 0;
};

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

inline bool requested()
{
  const char *value=std::getenv("BLENDER_SCULPT_XPBD_OTHER_BRUSHES_V15");
  return value && value[0]=='1' && value[1]=='\0' && assist_influence()>0.0;
}

/* If Blender's geometry mode changes after this physical brush has
 * already applied any dab, DO NOT mix in stock brush's different evaluator.
 * Return true to finish this stroke without changing geometry. */
inline bool freeze_if_mode_changed(Object &object, const bke::pbvh::Tree &pbvh)
{
  if (!requested() || pbvh.type()==bke::pbvh::Type::Mesh) return false;
  StrokeCache &cache=*object.runtime->sculpt_session->cache;
  if (!cache.xpbd_v15_active_logged) return false;
  cache.xpbd_v15_failed=true;
  return true;
}

inline bool try_apply(const Sculpt &sd,
                      Object &object,
                      const IndexMask &node_mask,
                      const PositionDeformData &position_data,
                      const sculpt_physics::NativeSculptBrushKind kind,
                      const std::vector<sculpt_physics::NativeBrushTranslation> &native_translations)
{
  if (!requested()) return false;
  SculptSession &ss=*object.runtime->sculpt_session;
  StrokeCache &cache=*ss.cache;
  if (cache.xpbd_v15_failed) return cache.xpbd_v15_active_logged;
  bke::pbvh::Tree &pbvh=*bke::object::pbvh_get(object);
  const ePaintSymmetryFlags sym=ePaintSymmetryFlags(
      PAINT_SYMM_X | PAINT_SYMM_Y | PAINT_SYMM_Z |
      PAINT_TILE_X | PAINT_TILE_Y | PAINT_TILE_Z);
  if (pbvh.type()!=bke::pbvh::Type::Mesh ||
      ss.deform_modifiers_active || ss.shapekey_active ||
      bke::multires_layers::base_modifier(object)!=nullptr ||
      (sd.paint.symmetry_flags & sym)!=PAINT_SYMM_NONE ||
      cache.tile_pass!=0 || cache.radial_symmetry_pass!=0 ||
      cache.mirror_symmetry_pass!=0) {
    // A previously active physical stroke must not switch abruptly to the
    // stock brush; freeze remaining dabs until the user releases the stroke.
    // Once a stroke chose native fallback, never enter the physical solver
    // later in that same stroke if the geometry configuration changes.
    cache.xpbd_v15_failed=true;
    return cache.xpbd_v15_active_logged;
  }
  const Mesh &mesh=*id_cast<Mesh *>(object.data);
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
  sculpt_physics::PreparedNativeBrushDab result;
  std::shared_ptr<NativePhysicsStroke> state;
  try {
    state=std::static_pointer_cast<NativePhysicsStroke>(cache.xpbd_v15_state);
    const Span<int2> edges=mesh.edges();
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
    if (samples.empty()) return true;
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
      if (compact_requested()) {
        state->compact=std::make_unique<sculpt_physics::CompactBrushTopology>(
            std::size_t(mesh.verts_num),std::size_t(mesh.edges_num),edge_at);
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
    dab.assist=assist_influence();
    dab.compliance=1.0e-6;
    // Compare against the old V15 baseline before enabling new profiles.
    static const bool profiles=[]() {
      const char *raw=std::getenv("BLENDER_SCULPT_XPBD_V15_PROFILES");
      return raw && raw[0]=='1' && raw[1]=='\0';
    }();
    if (profiles) {
      const auto profile=sculpt_physics::experimental_native_brush_profile(kind);
      dab.compliance=profile.target_compliance;
      dab.material=profile.material;
    }
    dab.topology_generation=1;
    std::vector<std::size_t> local_to_global;
    if (state->compact) {
      // The compact path stores connectivity only, and snapshots original
      // coordinates on first actual write. Untouched vertices retain their
      // stroke-start coordinates. No full-mesh physics World is allocated.
      auto prepared=sculpt_physics::prepare_compact_native_brush_dab(
          *state->compact,samples,edge_at,
          [&](std::size_t id) {
            const auto original=state->original_positions.find(id);
            return original==state->original_positions.end() ?
                       current_position(id) : original->second;
          },current_position,inverse_mass,dab);
      local_to_global=std::move(prepared.local_to_global);
      result=std::move(prepared.prepared);
    }
    else {
      result=sculpt_physics::prepare_cpu_native_brush_dab(
          state->rest_world,*state->topology,samples,current_position,dab);
    }

    // True zero-impact dab: don't traverse, clip or rewrite every PBVH
    // node with zero translations. Blender's ordinary smoothing/plane
    // clipping may otherwise modify stationary coordinates unexpectedly.
    if (result.writes().empty()) {
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
        if (!std::isfinite(got.x) || !std::isfinite(got.y) ||
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
  }
  catch (const std::exception &ex) {
    std::fprintf(stderr,"[V15 physical brush] disabled before write: %s\n",ex.what());
    result.cancel();
    cache.xpbd_v15_state.reset();
    cache.xpbd_v15_failed=true;
    return cache.xpbd_v15_active_logged;
  }

  // Full native Blender Undo journal/partial PBVH write rollback is not yet
  // implemented; this gated preview must not be enabled in release builds.
  for (PreparedNode &node : editor_writes) {
    bke::pbvh::MeshNode &pbvh_node=nodes[node.id];
    position_data.deform(node.translations,pbvh_node.verts());
    bke::pbvh::update_node_bounds_mesh(position_data.eval,pbvh_node);
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  result.finish_editor_success();
  cache.xpbd_v15_state=state;
  if (!cache.xpbd_v15_active_logged) {
    std::fprintf(stderr,"[V15 physical brush] CPU XPBD %s preview ACTIVE\n",
                 state->compact ? "compact" : "reference");
    cache.xpbd_v15_active_logged=true;
  }
  return true;
}

}  // namespace blender::ed::sculpt_paint::brushes::physics_v15
#endif
