/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

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
#include "editors/sculpt_paint/mesh/sculpt_layer_delta_smooth_v01.hh"

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

  /* Calculate the new positions into a separate array in a separate loop because multiple loops
   * are updated in parallel. Without this there would be non-threadsafe access to changing
   * positions in other bke::pbvh::Tree nodes. */
  for (const float strength : iteration_strengths(brush_strength)) {
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
              position_data.eval,
              verts,
              neighbors,
              new_positions.as_mutable_span().slice(node_vert_offsets[pos]));
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
            const float3 center = read_delta(coord);
            float3 translation(0.0f);
            for (const SubdivCCGCoord neighbor : neighbors.coords) {
              translation += read_delta(neighbor) - center;
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

