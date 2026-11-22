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
#ifdef WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14

#  include "native_grab_executor_adapter.hh"

#  include "BKE_mesh.hh"
#  include "BKE_multires_sculpt_layers.hh"
#  include "BKE_object_types.hh"
#  include "BKE_paint_bvh.hh"
#  include "DNA_scene_types.h"

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
  int verts_num = 0;
  int edges_num = 0;
};

inline bool requested()
{
  const char *value=std::getenv("BLENDER_SCULPT_XPBD_OTHER_BRUSHES_V15");
  return value && value[0]=='1' && value[1]=='\0';
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
    if (cache.xpbd_v15_active_logged) {
      cache.xpbd_v15_failed=true;
      return true;
    }
    return false;
  }
  const Mesh &mesh=*id_cast<Mesh *>(object.data);
  if (mesh.verts_num<=0 || position_data.eval.size()!=mesh.verts_num) return false;

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
    if (state && (state->verts_num!=mesh.verts_num ||
                  state->edges_num!=mesh.edges_num)) {
      throw std::logic_error("V15 topology count changed during a native stroke");
    }
    if (!state) {
      state=std::make_shared<NativePhysicsStroke>();
      state->verts_num=mesh.verts_num;
      state->edges_num=mesh.edges_num;
      const MeshAttributeData attributes(mesh);
      for (const int i : current.index_range()) {
        const float3 p=current[i];
        const bool hidden=!attributes.hide_vert.is_empty() && attributes.hide_vert[i];
        const float mask=attributes.mask.is_empty() ? 0.0f : attributes.mask[i];
        // Final native translation already contains mask attenuation. Only
        // pin fully masked/hidden vertices; do not apply partial mask twice.
        const double inv=hidden || mask>=0.999f ? 0.0 : 1.0;
        state->rest_world.add_particle({double(p.x),double(p.y),double(p.z)},inv);
      }
      for (const int2 edge : mesh.edges()) {
        try {
          state->rest_world.add_bond(
              std::size_t(edge[0]),std::size_t(edge[1]),1.0e-5);
        }
        catch (const std::invalid_argument &) {
          // Degenerate edges cannot define physical rest-length constraints.
        }
      }
      state->topology=std::make_unique<sculpt_physics::GpuTopologyBondIndex>(
          state->rest_world,1);
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
    for (const auto &item : dedup) samples.push_back({item.first,item.second});
    sculpt_physics::NativeBrushDab dab;
    dab.kind=kind;
    dab.compliance=1.0e-6;
    dab.topology_generation=1;
    result=sculpt_physics::prepare_cpu_native_brush_dab(
        state->rest_world,*state->topology,samples,
        [&](const std::size_t id)->sculpt_physics::Vec3 {
          if (id>=std::size_t(current.size())) throw std::out_of_range("V15 stale vertex ID");
          const float3 p=current[int(id)];
          return {double(p.x),double(p.y),double(p.z)};
        },dab);

    std::unordered_map<std::size_t,float3> changes;
    changes.reserve(result.writes().size());
    for (const auto &w : result.writes()) {
      const float3 p=current[int(w.global_index)];
      const float3 displacement{
          float(w.position.x-double(p.x)),
          float(w.position.y-double(p.y)),
          float(w.position.z-double(p.z))};
      if (!std::isfinite(displacement.x) ||
          !std::isfinite(displacement.y) ||
          !std::isfinite(displacement.z)) {
        throw std::runtime_error("V15 physics displacement out of float32 range");
      }
      changes.emplace(w.global_index,displacement);
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
    std::fprintf(stderr,"[V15 physical brush] CPU XPBD translation preview ACTIVE\n");
    cache.xpbd_v15_active_logged=true;
  }
  return true;
}

}  // namespace blender::ed::sculpt_paint::brushes::physics_v15
#endif
