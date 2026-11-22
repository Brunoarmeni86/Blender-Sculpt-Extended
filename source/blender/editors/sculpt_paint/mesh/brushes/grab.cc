/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <queue>
#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
#  include <algorithm>
#  include <cmath>
#  include <cstdio>
#  include <stdexcept>
#  include <utility>
#  include <cstdlib>
#  include <vector>
#  include "physics_core.hh"
#  ifdef WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14
#    include <unordered_map>
#    include <unordered_set>
#    include "native_grab_executor_adapter.hh"
#  endif
#endif

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
#  include "BKE_multires_sculpt_layers.hh"
#endif
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#include "bmesh.hh"

namespace blender {

namespace ed::sculpt_paint::brushes {
inline namespace grab_cc {

struct LocalData {
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

BLI_NOINLINE static void calc_silhouette_factors(const StrokeCache &cache,
                                                 const float3 &offset,
                                                 const Span<float3> normals,
                                                 const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(normals.size() == factors.size());

  const float sign = math::sign(math::dot(cache.initial_normal_symm, cache.grab_delta_symm));
  const float3 test_dir = math::normalize(offset) * sign;
  for (const int i : factors.index_range()) {
    factors[i] *= std::max(math::dot(test_dir, normals[i]), 0.0f);
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const float3 &offset,
                       const MeshAttributeData &attribute_data,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const OrigPositionData orig_data = orig_position_data_get_mesh(object, node);
  const Span<int> verts = node.verts();

  calc_factors_common_from_orig_data_mesh(depsgraph,
                                          brush,
                                          object,
                                          attribute_data,
                                          orig_data.positions,
                                          orig_data.normals,
                                          node,
                                          tls.factors,
                                          tls.distances);

  if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
    calc_silhouette_factors(cache, offset, orig_data.normals, tls.factors);
  }

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float3 &offset,
                       bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  const OrigPositionData orig_data = orig_position_data_get_grids(object, node);
  const Span<int> grids = node.grids();
  const int grid_verts_num = grids.size() * key.grid_area;

  calc_factors_common_from_orig_data_grids(depsgraph,
                                           brush,
                                           object,
                                           orig_data.positions,
                                           orig_data.normals,
                                           node,
                                           tls.factors,
                                           tls.distances);

  if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
    calc_silhouette_factors(cache, offset, orig_data.normals, tls.factors);
  }

  tls.translations.resize(grid_verts_num);
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, orig_data.positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float3 &offset,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  Array<float3> orig_positions(verts.size());
  Array<float3> orig_normals(verts.size());
  orig_position_data_gather_bmesh(*ss.bm_log, verts, orig_positions, orig_normals);

  calc_factors_common_from_orig_data_bmesh(
      depsgraph, brush, object, orig_positions, orig_normals, node, tls.factors, tls.distances);

  if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
    calc_silhouette_factors(cache, offset, orig_normals, tls.factors);
  }

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, orig_positions, translations);
  apply_translations(translations, verts);
}

#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
/* V13: deliberately conservative persistent native-scene integration.
 * Runtime OFF by default, normal PBVH Mesh only. The physics proxy is
 * constructed once per Blender stroke and released with StrokeCache.
 * The active set is restricted to vertices in the existing sculpt node mask
 * to avoid writing unrecorded vertices outside Blender's native Undo region.
 * This is NOT yet the final multi-island/Multires integration. */
static bool try_xpbd_grab_preview(const Depsgraph &depsgraph,
                                  const Sculpt &sd,
                                  Object &object,
                                  const IndexMask &node_mask,
                                  const Brush &brush,
                                  const float3 &grab_delta,
                                  bke::pbvh::Tree &pbvh)
{
  const char *enabled = std::getenv("BLENDER_SCULPT_XPBD_GRAB_V12");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') return false;
  if (pbvh.type() != bke::pbvh::Type::Mesh) return false;
  SculptSession &ss = *object.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  // Do not bypass Sculpt Extended's Multires base-layer recording/CCG logic.
  // This first gate is only for an unmodified ordinary mesh object.
  if (bke::multires_layers::base_modifier(object) != nullptr) return false;
  // PAINT_SYMMETRY_FEATHER is ON by default even with no symmetry axes.
  // Reject only actual mirror/tile axes, not the harmless feather option.
  const ePaintSymmetryFlags nontrivial_symmetry = ePaintSymmetryFlags(
      PAINT_SYMM_X | PAINT_SYMM_Y | PAINT_SYMM_Z |
      PAINT_TILE_X | PAINT_TILE_Y | PAINT_TILE_Z);
  if (cache.xpbd_v12_failed || ss.deform_modifiers_active || ss.shapekey_active ||
      (sd.paint.symmetry_flags & nontrivial_symmetry) != PAINT_SYMM_NONE ||
      cache.tile_pass != 0 ||
      cache.radial_symmetry_pass != 0 || cache.mirror_symmetry_pass != 0)
  {
    return false;
  }

  const Mesh &mesh = *id_cast<Mesh *>(object.data);
  // V13 persistent proxy: no fixed vertex ceiling. Allocation or solver failures
  // remain transactional and fall back to Blender's standard Grab path.
  if (mesh.verts_num == 0) return false;
  const PositionDeformData position_data(depsgraph, object);
  const Span<float3> eval = position_data.eval;
  if (eval.size() != mesh.verts_num) return false;
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const MeshAttributeData attributes(mesh);
  const float3 delta = grab_delta - cache.xpbd_v12_previous_delta;
  const double h = 1.0 / 60.0;

  struct Influence {
    int vertex;
    float factor;
  };
  struct PreparedNode {
    int node_id;
    Vector<float3> translation;
  };
  std::vector<PreparedNode> writes;
  try {
    std::shared_ptr<sculpt_physics::World> world =
        std::static_pointer_cast<sculpt_physics::World>(cache.xpbd_v12_world);
    if (!world || cache.xpbd_v12_world_verts != mesh.verts_num) {
      world = std::make_shared<sculpt_physics::World>();
      for (const int i : eval.index_range()) {
        const float3 p = eval[i];
        const bool hidden = !attributes.hide_vert.is_empty() && attributes.hide_vert[i];
        const float mask = attributes.mask.is_empty() ? 0.0f : attributes.mask[i];
        const double inv_mass = hidden || mask >= 0.999f ? 0.0 :
                                std::max(1.0e-4, 1.0 - double(mask));
        world->add_particle({double(p.x), double(p.y), double(p.z)}, inv_mass);
      }
      for (const int2 edge : mesh.edges()) {
        // Degenerate source edges do not define a valid material constraint.
        try {
          world->add_bond(std::size_t(edge[0]), std::size_t(edge[1]), 1.0e-5);
        }
        catch (const std::invalid_argument &) {
          continue;
        }
      }
      world->begin_stroke();
      cache.xpbd_v12_world = world;
      cache.xpbd_v12_world_verts = mesh.verts_num;
    }
    std::vector<Influence> selection;
    Vector<float> factors;
    Vector<float> distances;
    node_mask.foreach_index([&](const int ni) {
      const bke::pbvh::MeshNode &node = nodes[ni];
      const OrigPositionData orig_data = orig_position_data_get_mesh(object, node);
      const Span<int> verts = node.verts();
      calc_factors_common_from_orig_data_mesh(depsgraph,
                                              brush,
                                              object,
                                              attributes,
                                              orig_data.positions,
                                              orig_data.normals,
                                              node,
                                              factors,
                                              distances);
      if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
        calc_silhouette_factors(cache, grab_delta, orig_data.normals, factors);
      }
      for (const int j : verts.index_range()) {
        if (factors[j] > 0.0f && std::isfinite(factors[j])) {
          selection.push_back({verts[j], std::min(factors[j], 1.0f)});
        }
      }
    });
    std::sort(selection.begin(), selection.end(),
              [](const Influence &a, const Influence &b) { return a.vertex < b.vertex; });
    std::vector<sculpt_physics::World::BrushTargetInput> targets;
    for (std::size_t k = 0; k < selection.size();) {
      const int vi = selection[k].vertex;
      float strength = selection[k].factor;
      while (++k < selection.size() && selection[k].vertex == vi) {
        strength = std::max(strength, selection[k].factor);
      }
      const float3 source = eval[vi];
      const float3 goal = source + delta * strength;
      targets.push_back({std::size_t(vi), {double(goal.x),double(goal.y),double(goal.z)}, 1.0e-6});
    }
    if (targets.empty()) {
      cache.xpbd_v12_previous_delta = grab_delta;
      return true;
    }
    world->set_brush_targets(targets);
    // Only brush targets (hop=0) are dynamic. Crossing bonds react as
    // numerical pins; no exterior mesh vertex can change without an Undo node.
    world->step_local(h, 2, 5, 0);

    // Complete staging and float-range validation before any editor writeback.
    // Use the *same* PBVH node mask already handled by Blender Sculpt Undo.
    node_mask.foreach_index([&](const int ni) {
      PreparedNode prepared;
      prepared.node_id = ni;
      const Span<int> verts = nodes[ni].verts();
      prepared.translation.resize(verts.size());
      for (const int j : verts.index_range()) {
        const sculpt_physics::Vec3 desired = world->particles()[verts[j]].position;
        const float3 old = eval[verts[j]];
        const float3 change{float(desired.x-double(old.x)),
                            float(desired.y-double(old.y)),
                            float(desired.z-double(old.z))};
        if (!std::isfinite(change.x) || !std::isfinite(change.y) || !std::isfinite(change.z)) {
          throw std::runtime_error("V12 experimental Grab displacement out of float range");
        }
        prepared.translation[j] = change;
      }
      clip_and_lock_translations(sd, ss, eval, verts, prepared.translation);
      writes.push_back(std::move(prepared));
    });
  }
  catch (const std::exception &ex) {
    // No mesh was changed: hand this dab back to the regular Grab brush,
    // and disable the experimental preview for the rest of this stroke.
    std::fprintf(stderr, "[V12 XPBD Grab] preview disabled: %s\n", ex.what());
    cache.xpbd_v12_world.reset();
    cache.xpbd_v12_world_verts = 0;
    cache.xpbd_v12_failed = true;
    return false;
  }

  for (PreparedNode &prepared : writes) {
    bke::pbvh::MeshNode &node = nodes[prepared.node_id];
    position_data.deform(prepared.translation, node.verts());
    bke::pbvh::update_node_bounds_mesh(position_data.eval, node);
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  if (!cache.xpbd_v12_active_logged) {
    std::fprintf(stderr, "[V12 XPBD Grab] experimental preview ACTIVE on ordinary mesh\n");
    cache.xpbd_v12_active_logged = true;
  }
  cache.xpbd_v12_previous_delta = grab_delta;
  return true;
}
#endif

}  // namespace grab_cc

void do_grab_brush(const Depsgraph &depsgraph,
                   const Sculpt &sd,
                   Object &object,
                   const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  const float3 grab_delta = grab_delta_get(brush, *ss.cache) * ss.cache->bstrength;

#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
  if (try_xpbd_grab_preview(depsgraph, sd, object, node_mask, brush, grab_delta, pbvh)) {
    return;
  }
#endif

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh);
      const PositionDeformData position_data(depsgraph, object);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_faces(depsgraph,
                       sd,
                       brush,
                       grab_delta,
                       attribute_data,
                       nodes[i],
                       object,
                       tls,
                       position_data);
            bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_grids(depsgraph, sd, object, brush, grab_delta, nodes[i], tls);
            bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_bmesh(depsgraph, sd, object, brush, grab_delta, nodes[i], tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}
}  // namespace ed::sculpt_paint::brushes

namespace ed::sculpt_paint {

void geometry_preview_lines_update(Depsgraph &depsgraph,
                                   Object &object,
                                   SculptSession &ss,
                                   float radius)
{
  ss.preview_verts = {};

  /* This function is called from the cursor drawing code, so the tree may not be built yet. */
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh) {
    return;
  }

  if (!ss.deform_modifiers_active) {
    return;
  }

  if (pbvh->type() != bke::pbvh::Type::Mesh) {
    return;
  }

  BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);

  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  /* Always grab active shape key if the sculpt happens on shapekey. */
  const Span<float3> positions = ss.shapekey_active ?
                                     bke::pbvh::vert_positions_eval(depsgraph, object) :
                                     mesh.vert_positions();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);

  const int active_vert = std::get<int>(ss.active_vert());
  const float3 brush_co = positions[active_vert];
  const float radius_sq = radius * radius;

  Vector<int> preview_verts;
  Vector<int> neighbors;
  BitVector<> visited_verts(positions.size());
  std::queue<int> queue;
  queue.push(active_vert);
  while (!queue.empty()) {
    const int from_vert = queue.front();
    queue.pop();

    neighbors.clear();
    for (const int neighbor : vert_neighbors_get_mesh(
             faces, corner_verts, vert_to_face_map, hide_poly, from_vert, neighbors))
    {
      preview_verts.append(from_vert);
      preview_verts.append(neighbor);
      if (visited_verts[neighbor]) {
        continue;
      }
      visited_verts[neighbor].set();
      if (math::distance_squared(brush_co, positions[neighbor]) < radius_sq) {
        queue.push(neighbor);
      }
    }
  }

  ss.preview_verts = preview_verts.as_span();
}

}  // namespace ed::sculpt_paint

}  // namespace blender
