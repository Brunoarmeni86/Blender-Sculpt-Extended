/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_modifier_types.h"
#include "smooth_preserve_shape.hh"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_eval.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include <cstdlib>
#include <cmath>
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_boundary.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"
#include "editors/sculpt_paint/mesh/sculpt_smooth.hh"

#include "bmesh.hh"

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
  /* V01 opt-in: offsets cached only for visited CCG samples/neighbor halo. */
  Map<int, float3> isolated_other_offsets;
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

/* SCULPT_SMOOTH_ISOLATED_V01
 * Experimental, layer-aware Smooth. Instead of modifying the live CCG or
 * allocating a second dense surface, create virtual positions by removing
 * the other visible layers' object-space displacement at queried samples.
 *
 * CCG seams combine several tangent frames. V01 deliberately excludes the
 * first/last two rows and columns of each grid from editing, so no unverified
 * seam-space interpolation is used as a writable smoothing target.
 *
 * At the top sculpt level the layer samples use the same per-corner
 * tangent frames as the Multires displacement evaluator. */
struct IsolatedSmoothContext {
  const SubdivCCG &ccg;
  const CCGKey key;
  Vector<const MultiresSculptLayer *> other_layers;
  Span<int> ptex_offsets;
};

static bool smooth_isolated_context_build(const Object &object,
                                          const SubdivCCG &ccg,
                                          IsolatedSmoothContext &context)
{
  const char *flag = std::getenv("BLENDER_SCULPT_SMOOTH_ISOLATED");
  if (!flag || flag[0] != '1' || flag[1] != '\0') {
    return false;
  }
  const SculptSession *ss = object.runtime->sculpt_session;
  const MultiresModifierData *mmd = ss ? ss->multires_modifier : nullptr;
  const Mesh *mesh = object.type == OB_MESH ? static_cast<const Mesh *>(object.data) : nullptr;
  if (!mmd || !mesh || !ccg.subdiv || ccg.level != mmd->totlvl ||
      ccg.level != mmd->sculptlvl || ccg.grid_size < 9 ||
      ccg.grids_num != mesh->corners_num || !mmd->sculpt_layer_recording ||
      mmd->sculpt_displacement_enabled || mmd->sculpt_shrinkwrap_enabled)
  {
    return false;
  }
  const MultiresSculptLayer *active = bke::multires_layers::active(*mmd);
  if (!active || !bke::multires_layers::is_effective(*active) ||
      active->level != mmd->totlvl || active->grids_num != ccg.grids_num)
  {
    return false;
  }
  for (const MultiresSculptLayer &other : mmd->sculpt_layers) {
    if (&other == active || !bke::multires_layers::is_effective(other)) {
      continue;
    }
    /* L0 moves the cage/limit surface; it cannot be removed using a grid
     * tangent-space offset without rebasing. Disable isolation conservatively. */
    if (other.base_deltas != nullptr || other.disps == nullptr ||
        other.grids_num != ccg.grids_num || other.level < 1)
    {
      return false;
    }
    context.other_layers.append(&other);
  }
  context.ptex_offsets = bke::subdiv::face_ptex_offset_get(ccg.subdiv);
  return !context.other_layers.is_empty() &&
         context.ptex_offsets.size() == ccg.faces.size() + 1;
}

static float3 smooth_other_layer_offset(const IsolatedSmoothContext &context,
                                        const int index,
                                        Map<int, float3> &cache)
{
  if (const float3 *cached = cache.lookup_ptr(index)) {
    return *cached;
  }
  const SubdivCCG &ccg = context.ccg;
  const SubdivCCGCoord coord = SubdivCCGCoord::from_index(context.key, index);
  const int face_index = BKE_subdiv_ccg_grid_to_face_index(ccg, coord.grid_index);
  const IndexRange face = ccg.faces[face_index];
  const int corner = coord.grid_index - face.start();
  const float grid_u = float(coord.x) / float(ccg.grid_size - 1);
  const float grid_v = float(coord.y) / float(ccg.grid_size - 1);

  float3 weighted_tangent(0.0f);
  for (const MultiresSculptLayer *layer : context.other_layers) {
    float sampled[3];
    if (bke::multires_layers::sample_tangent(
            *layer, coord.grid_index, ccg.grid_size, grid_u, grid_v, sampled))
    {
      weighted_tangent += float3(sampled[0], sampled[1], sampled[2]);
    }
  }
  if (math::is_zero(weighted_tangent)) {
    cache.add(index, float3(0.0f));
    return float3(0.0f);
  }

  const bool quad = face.size() == 4;
  const int ptex_face = context.ptex_offsets[face_index] + (quad ? 0 : corner);
  float u, v;
  if (quad) {
    bke::subdiv::rotate_grid_to_quad(corner, grid_u, grid_v, &u, &v);
  }
  else {
    bke::subdiv::grid_uv_to_ptex_face_uv(grid_u, grid_v, &u, &v);
  }
  float3 P, dPdu, dPdv;
  bke::subdiv::eval_limit_point_and_derivatives(
      ccg.subdiv, ptex_face, u, v, P, dPdu, dPdv);
  float3x3 tangent;
  BKE_multires_construct_tangent_matrix(tangent, dPdu, dPdv, quad ? corner : 0);
  const float3 offset = math::transform_direction(tangent, weighted_tangent);
  cache.add(index, offset);
  return offset;
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
                       LocalData &tls,
                       const IsolatedSmoothContext *isolation)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  calc_factors_common_grids(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  scale_factors(tls.factors, strength);

  /* Seam-safe V01: no edits in or within two samples of CCG grid borders.
   * Neighbor reads for writable interior samples thus remain inside their
   * own tangent chart. A future version will handle stitched seams. */
  if (isolation) {
    const int side = subdiv_ccg.grid_size;
    const int area = subdiv_ccg.grid_area;
    for (const int grid_i : grids.index_range()) {
      for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
          if (x < 2 || y < 2 || x >= side - 2 || y >= side - 2) {
            tls.factors[grid_i * area + y * side + x] = 0.0f;
          }
        }
      }
    }
  }

  tls.new_positions.resize(positions.size());
  const MutableSpan<float3> new_positions = tls.new_positions;

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;

  if (isolation) {
    const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
    const auto read_isolated = [&](const int index) -> float3 {
      return subdiv_ccg.positions[index] -
             smooth_other_layer_offset(*isolation, index, tls.isolated_other_offsets);
    };
    smooth::neighbor_position_average_interior_grids_isolated(faces,
                                                              corner_verts,
                                                              boundary_verts,
                                                              boundary_edges,
                                                              subdiv_ccg,
                                                              grids,
                                                              tls.factors,
                                                              read_isolated,
                                                              new_positions);
    int node_sample = 0;
    for (const int grid : grids) {
      const int base_index = grid * key.grid_area;
      for (int i = 0; i < key.grid_area; i++, node_sample++) {
        if (tls.factors[node_sample] == 0.0f) {
          translations[node_sample] = float3(0.0f);
          continue;
        }
        const float3 center = read_isolated(base_index + i);
        translations[node_sample] = new_positions[node_sample] - center;
      }
    }
  }
  else {
    smooth::neighbor_position_average_interior_grids(faces,
                                                     corner_verts,
                                                     boundary_verts,
                                                     boundary_edges,
                                                     subdiv_ccg,
                                                     grids,
                                                     tls.factors,
                                                     new_positions);
    translations_from_new_positions(new_positions, positions, translations);
  }
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
      const Mesh &base_mesh = *id_cast<const Mesh *>(object.data);
      const OffsetIndices faces = base_mesh.faces();
      const Span<int> corner_verts = base_mesh.corner_verts();

      threading::EnumerableThreadSpecific<LocalData> all_tls;
      IsolatedSmoothContext isolated_context{
          *ss.subdiv_ccg, BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg), {}, {}};
      const bool isolation_enabled =
          smooth_isolated_context_build(object, *ss.subdiv_ccg, isolated_context) &&
          !(brush.flag2 & BRUSH_SCULPT_SMOOTH_SHAPE_PRESERVE);
      const char *isolated_trace = std::getenv("BLENDER_SCULPT_SMOOTH_ISOLATED_TRACE");
      if (isolated_trace && isolated_trace[0] == '1' && isolated_trace[1] == '\0') {
        std::fprintf(stderr,
                     "[SMOOTH_ISOLATED_V01] enabled=%d others=%lld sculpt=%d grid_side=%d\n",
                     int(isolation_enabled),
                     static_cast<long long>(isolated_context.other_layers.size()),
                     ss.subdiv_ccg->level,
                     ss.subdiv_ccg->grid_size);
      }
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
                         tls,
                         isolation_enabled ? &isolated_context : nullptr);
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
