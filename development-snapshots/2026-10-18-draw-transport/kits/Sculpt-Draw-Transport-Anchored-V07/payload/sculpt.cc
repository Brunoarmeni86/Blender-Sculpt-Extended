/* SPDX-FileCopyrightText: 2006 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Implements the Sculpt Mode Brushes.
 */

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#endif
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "sculpt_layer_delta_smooth_v01.hh"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <limits>
#include <stdexcept>
#include <queue>
#include <unordered_map>
#include <vector>

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "BLI_time.h"
#include "BLI_kdopbvh.hh"
#include "BLI_array_utils.hh"
#include "BLI_atomic_disjoint_set.hh"
#include "BLI_dial_2d.h"
#include "BLI_enum_flags.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_listbase.h"
#include "BLI_math_axis_angle.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_rotation_legacy.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_customdata_types.h"
#include "DNA_key_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_ccg.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_sculpt_cold_start_profile.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_undo_system.hh"

#include "BLT_translation.hh"

#include "NOD_texture.h"

#include "PRF_profile.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "../paint_intern.hh"
#include "sculpt_automask.hh"
#include "sculpt_boundary.hh"
#include "sculpt_cloth.hh"
#include "sculpt_color.hh"
#include "sculpt_dyntopo.hh"
#include "sculpt_face_set.hh"
#include "sculpt_filter.hh"
#include "sculpt_hide.hh"
#include "sculpt_intern.hh"
#include "sculpt_islands.hh"
#include "sculpt_pose.hh"
#include "sculpt_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"
#include "mesh_brush_common.hh"

namespace blender {

static CLG_LogRef LOG = {"sculpt"};

namespace ed::sculpt_paint {

/* ANCHORED_GEODESIC_V3: use memory proportional to the stamp, never the complete mesh.
 * Allocated and updated before parallel brush execution, then only read by workers. */
/* ANCHORED_DIAGNOSTICS_V41: opt-in, stroke-owned; no DNA or geometry writes.
 * Sample counters deliberately measure texture calls, not unique vertices. */
struct AnchorProjectionDiagnostics {
  int level = 0;
  const char *reason = "ineligible";
  const char *transport_reason = "disabled";
  const char *cache_event = "new";
  float seed_normal_dot = 0.0f, seed_y_dot = 0.0f;
  double prepare_ms = 0.0, search_ms = 0.0, relax_ms = 0.0, audit_ms = 0.0;
  size_t visited = 0, frames = 0, popped = 0, relaxed = 0;
  size_t failed_transport = 0, nonfinite_uv = 0, seam_edges = 0;
  size_t seam_pairs = 0, seam_missing = 0;
  float seam_uv_max = 0.0f, tangent_error_max = 0.0f;
  bool requested = false, built_transport = false;
  int stride = 1, refinements = 0, relax_iterations = 0;
  int seed_count = 1;
  float source_error = 0.0f;
  double metric_before = 0.0, metric_after = 0.0;
  std::atomic<uint64_t> samples[7]{}; /* planar, V1, V2, V3, V4, rejected, V5. */
  std::atomic<uint64_t> missing_ccg{0};
};

static int anchor_diagnostic_level()
{
  /* Read once per process; disabled path has no sample atomics or audit traversal. */
  static const int level = []() {
    const char *value = std::getenv("BLENDER_ANCHOR_DIAGNOSTICS");
    return value && value[0] == '2' ? 2 : (value && value[0] == '1' ? 1 : 0);
  }();
  return level;
}

struct AnchorDiagnosticTimer {
  AnchorProjectionDiagnostics *diag;
  double start;
  ~AnchorDiagnosticTimer()
  {
    if (diag) {
      diag->prepare_ms = (BLI_time_now_seconds() - start) * 1000.0;
    }
  }
};

static void anchor_diagnostic_report(const StrokeCache &cache)
{
  const auto *d = cache.anchor_projection_diagnostics.get();
  if (!d) {
    return;
  }
  std::printf("[ANCHOR_DIAG] reason=%s transport=%s cache=%s requested=%d built=%d "
              "pass=%d/%d/%d radius=%.9g prepare_ms=%.3f search_ms=%.3f relax_ms=%.3f audit_ms=%.3f "
              "visited=%zu frames=%zu popped=%zu relaxed=%zu "
              "transport_fail=%zu uv_nonfinite=%zu seam_edges=%zu "
              "seam_pairs=%zu seam_missing=%zu seam_uv_max=%.9g tangent_error=%.9g "
              "seed_normal_dot=%.6g seed_y_dot=%.6g "
              "stride=%d refinements=%d relax_iterations=%d metric_before=%.6g metric_after=%.6g "
              "seed_count=%d source_error=%.9g "
              "planar=%llu v1=%llu v2=%llu v3=%llu v4=%llu rejected=%llu v5=%llu missing_ccg=%llu\n",
              d->reason, d->transport_reason, d->cache_event,
              int(d->requested), int(d->built_transport),
              int(cache.mirror_symmetry_pass), cache.radial_symmetry_pass, cache.tile_pass,
              double(cache.radius), d->prepare_ms, d->search_ms, d->relax_ms, d->audit_ms,
              d->visited, d->frames, d->popped, d->relaxed,
              d->failed_transport, d->nonfinite_uv, d->seam_edges,
              d->seam_pairs, d->seam_missing, double(d->seam_uv_max),
              double(d->tangent_error_max), double(d->seed_normal_dot), double(d->seed_y_dot),
              d->stride, d->refinements, d->relax_iterations, d->metric_before, d->metric_after,
              d->seed_count, double(d->source_error),
              (unsigned long long)d->samples[0].load(std::memory_order_relaxed),
              (unsigned long long)d->samples[1].load(std::memory_order_relaxed),
              (unsigned long long)d->samples[2].load(std::memory_order_relaxed),
              (unsigned long long)d->samples[3].load(std::memory_order_relaxed),
              (unsigned long long)d->samples[4].load(std::memory_order_relaxed),
              (unsigned long long)d->samples[5].load(std::memory_order_relaxed),
              (unsigned long long)d->samples[6].load(std::memory_order_relaxed),
              (unsigned long long)d->missing_ccg.load(std::memory_order_relaxed));
}

static bool anchor_safe_normalize(float3 &value)
{
  const float length = normalize_v3(value);
  return std::isfinite(length) && length > 1.0e-6f &&
         std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

struct AnchorSurfacePathFrame {
  float2 uv = float2(0.0f);
  float3 tangent_x = float3(0.0f);
};

struct AnchorV5Sample {
  float distance = 0.0f;
  float2 uv = float2(0.0f);
  bool has_uv = true;
};
/* V5.1: sparse 8x8 control blocks. Most bilinear samples need one hash lookup,
 * and block occupancy keeps missing/unreachable controls explicit. */
struct AnchorV51ControlBlock {
  std::array<AnchorV5Sample, 64> values;
  uint64_t present = 0;
};

struct AnchorSurfacePathCache {
  const SubdivCCG *geometry = nullptr;
  float3 origin = float3(0.0f);
  float coverage_radius = 0.0f;
  int mirror_pass = -1;
  int radial_pass = -1;
  int tile_pass = -1;
  bool ready = false;
  bool transport_ready = false;
  float3 reference_axis = float3(0.0f);
  bool projection_v5 = false, projection_v52 = false, radial_correction = false;
  int preferred_stride = 0, seed_count = 1, source_grid = -1;
  float source_u = 0.0f, source_v = 0.0f, source_error = 0.0f;
  std::array<int, 4> root_controls = {-1, -1, -1, -1};
  const float3 *position_buffer = nullptr, *normal_buffer = nullptr;
  int64_t position_count = 0, normal_count = 0;
  int blocks_per_row = 0, stride_shift = 0, last_control = 0;
  float inverse_stride = 1.0f;
  int stride = 1, grid_size = 0, grid_area = 0, refinements = 0, relax_iterations = 0;
  float handedness = 1.0f;
  float3 reference_normal = float3(0.0f);
  float2 uv_rotation = {1.0f, 0.0f};
  double metric_before = 0.0, metric_after = 0.0, relax_ms = 0.0;
  std::unordered_map<uint64_t, AnchorV51ControlBlock> control_blocks;
  std::unordered_map<int, int> aliases;
  std::unordered_map<int, float> distances;
  /* Only populated in V4 and bounded to the current stamp, not the whole CCG. */
  std::unordered_map<int, AnchorSurfacePathFrame> frames;
};

static bool anchor_v5_sample(const AnchorSurfacePathCache &path,
                             int ccg_index,
                             AnchorV5Sample &sample);

/* -------------------------------------------------------------------- */
/** \name Sculpt Brush Utilities
 * \{ */

/* TODO: This should be moved to either BKE_paint.hh or BKE_brush.hh */
float object_space_radius_get(const ViewContext &vc,
                              const Paint &paint,
                              const Brush &brush,
                              const float3 &location,
                              const float scale_factor)
{
  if (!BKE_brush_use_locked_size(&paint, &brush)) {
    return paint_calc_object_space_radius(
        vc, location, BKE_brush_radius_get(&paint, &brush) * scale_factor);
  }
  return BKE_brush_unprojected_radius_get(&paint, &brush) * scale_factor;
}

bool shape_key_check(const Object &ob, ReportList *reports)
{
  SculptSession &ss = *ob.runtime->sculpt_session;

  if (ss.shapekey_active && (ss.shapekey_active->flag & KEYBLOCK_LOCKED_SHAPE) != 0) {
    if (reports) {
      BKE_reportf(reports, RPT_ERROR, "The active shape key of %s is locked", ob.id.name + 2);
    }
    return false;
  }
  if (ss.shapekey_active && (ss.shapekey_active->flag & KEYBLOCK_MUTE) != 0) {
    if (reports) {
      BKE_reportf(reports, RPT_ERROR, "The active shape key of %s is muted", ob.id.name + 2);
    }
    return false;
  }

  return true;
}

void vert_random_access_ensure(Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  if (bke::object::pbvh_get(object)->type() == bke::pbvh::Type::BMesh) {
    BM_mesh_elem_index_ensure(ss.bm, BM_VERT);
    BM_mesh_elem_table_ensure(ss.bm, BM_VERT);
  }
}

int vertex_count_get(const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh:
      BLI_assert(object.type == OB_MESH);
      return id_cast<const Mesh *>(object.data)->verts_num;
    case bke::pbvh::Type::BMesh:
      return BM_mesh_elem_count(ss.bm, BM_VERT);
    case bke::pbvh::Type::Grids:
      return BKE_sculpt_get_grid_num_verts(object);
  }

  return 0;
}

Span<float3> vert_positions_for_grab_active_get(const Depsgraph &depsgraph, const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  BLI_assert(bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Mesh);
  if (ss.shapekey_active) {
    /* Always grab active shape key if the sculpt happens on shapekey. */
    return bke::pbvh::vert_positions_eval(depsgraph, object);
  }
  /* Otherwise use the base mesh positions. */
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  return mesh.vert_positions();
}

ePaintSymmetryFlags mesh_symmetry_xyz_get(const Object &object)
{
  const Mesh *mesh = id_cast<const Mesh *>(object.data);
  return ePaintSymmetryFlags(mesh->symmetry);
}

/* Sculpt Face Sets and Visibility. */

namespace face_set {

int active_face_set_get(const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArray face_sets = *attributes.lookup<int>(".sculpt_face_set", bke::AttrDomain::Face);
      if (!face_sets || !ss.active_face_index) {
        return face_set_none_id;
      }
      return face_sets[*ss.active_face_index];
    }
    case bke::pbvh::Type::Grids: {
      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArray face_sets = *attributes.lookup<int>(".sculpt_face_set", bke::AttrDomain::Face);
      if (!face_sets || !ss.active_grid_index) {
        return face_set_none_id;
      }
      const int face_index = BKE_subdiv_ccg_grid_to_face_index(*ss.subdiv_ccg,
                                                               *ss.active_grid_index);
      return face_sets[face_index];
    }
    case bke::pbvh::Type::BMesh:
      return face_set_none_id;
  }
  return face_set_none_id;
}

int vert_face_set_max_get(const GroupedSpan<int> vert_to_face_map,
                          const Span<int> face_sets,
                          const int vert)
{
  int face_set = face_set_none_id;
  for (const int face : vert_to_face_map[vert]) {
    face_set = std::max(face_sets[face], face_set);
  }
  return face_set;
}

int vert_face_set_get(const SubdivCCG &subdiv_ccg, const Span<int> face_sets, const int grid)
{
  const int face = BKE_subdiv_ccg_grid_to_face_index(subdiv_ccg, grid);
  return face_sets[face];
}

int vert_face_set_max_get(const int /*face_set_offset*/, const BMVert & /*vert*/)
{
  return face_set_none_id;
}

Set<int> vert_face_sets_get(const GroupedSpan<int> vert_to_face_map,
                            const Span<int> face_sets,
                            const int vert)
{
  Set<int> result;
  for (const int face : vert_to_face_map[vert]) {
    result.add(face_sets[face]);
  }
  if (result.is_empty()) {
    result.add(face_set_none_id);
  }
  return result;
}

bool vert_has_face_set(const GroupedSpan<int> vert_to_face_map,
                       const Span<int> face_sets,
                       const int vert,
                       const int face_set)
{
  if (face_sets.is_empty()) {
    return face_set == face_set_none_id;
  }
  const Span<int> faces = vert_to_face_map[vert];
  return std::any_of(
      faces.begin(), faces.end(), [&](const int face) { return face_sets[face] == face_set; });
}

bool vert_has_face_set(const SubdivCCG &subdiv_ccg,
                       const Span<int> face_sets,
                       const int grid,
                       const int face_set)
{
  if (face_sets.is_empty()) {
    return face_set == face_set_none_id;
  }
  const int face = BKE_subdiv_ccg_grid_to_face_index(subdiv_ccg, grid);
  return face_sets[face] == face_set;
}

bool vert_has_face_set(const int face_set_offset, const BMVert &vert, const int face_set)
{
  if (face_set_offset == -1) {
    return face_set == face_set_none_id;
  }
  BMIter iter;
  BMFace *face;
  BM_ITER_ELEM (face, &iter, &const_cast<BMVert &>(vert), BM_FACES_OF_VERT) {
    if (BM_ELEM_CD_GET_INT(face, face_set_offset) == face_set) {
      return true;
    }
  }
  return false;
}

bool vert_has_any_face_set(const GroupedSpan<int> vert_to_face_map,
                           const Span<int> face_sets,
                           int vert,
                           const Set<int> &allowed_face_sets)
{
  if (face_sets.is_empty()) {
    return allowed_face_sets.contains(face_set_none_id);
  }
  for (const int face : vert_to_face_map[vert]) {
    if (allowed_face_sets.contains(face_sets[face])) {
      return true;
    }
  }
  return false;
}

bool vert_has_unique_face_set(const GroupedSpan<int> vert_to_face_map,
                              const Span<int> face_sets,
                              int vert)
{
  /* TODO: Move this check higher out of this function. */
  if (face_sets.is_empty()) {
    return true;
  }
  int face_set = -1;
  for (const int face_index : vert_to_face_map[vert]) {
    if (face_set == -1) {
      face_set = face_sets[face_index];
    }
    else {
      if (face_sets[face_index] != face_set) {
        return false;
      }
    }
  }
  return true;
}

/**
 * Checks if the face sets of the adjacent faces to the edge between \a v1 and \a v2
 * in the base mesh are equal.
 */
static bool sculpt_check_unique_face_set_for_edge_in_base_mesh(
    const GroupedSpan<int> vert_to_face_map,
    const Span<int> face_sets,
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    int v1,
    int v2)
{
  const Span<int> vert_map = vert_to_face_map[v1];
  int p1 = -1, p2 = -1;
  for (int i = 0; i < vert_map.size(); i++) {
    const int face_i = vert_map[i];
    for (const int corner : faces[face_i]) {
      if (corner_verts[corner] == v2) {
        if (p1 == -1) {
          p1 = vert_map[i];
          break;
        }

        if (p2 == -1) {
          p2 = vert_map[i];
          break;
        }
      }
    }
  }

  if (p1 != -1 && p2 != -1) {
    return face_sets[p1] == face_sets[p2];
  }
  return true;
}

bool vert_has_unique_face_set(const OffsetIndices<int> faces,
                              const Span<int> corner_verts,
                              const GroupedSpan<int> vert_to_face_map,
                              const Span<int> face_sets,
                              const SubdivCCG &subdiv_ccg,
                              SubdivCCGCoord coord)
{
  /* TODO: Move this check higher out of this function. */
  if (face_sets.is_empty()) {
    return true;
  }
  int v1, v2;
  const SubdivCCGAdjacencyType adjacency = BKE_subdiv_ccg_coarse_mesh_adjacency_info_get(
      subdiv_ccg, coord, corner_verts, faces, v1, v2);
  switch (adjacency) {
    case SubdivCCGAdjacencyType::Vertex:
      return vert_has_unique_face_set(vert_to_face_map, face_sets, v1);
    case SubdivCCGAdjacencyType::Edge:
      return sculpt_check_unique_face_set_for_edge_in_base_mesh(
          vert_to_face_map, face_sets, corner_verts, faces, v1, v2);
    case SubdivCCGAdjacencyType::None:
      return true;
  }
  BLI_assert_unreachable();
  return true;
}

bool coord_has_face_set(const OffsetIndices<int> faces,
                        const Span<int> corner_verts,
                        const GroupedSpan<int> vert_to_face_map,
                        const Span<int> face_sets,
                        const SubdivCCG &subdiv_ccg,
                        const SubdivCCGCoord coord,
                        const int face_set)
{
  if (face_sets.is_empty()) {
    return face_set == face_set_none_id;
  }

  if (face_set == face_set_none_id) {
    return false;
  }

  Set<int> allowed_face_sets;
  allowed_face_sets.add(face_set);
  return coord_has_any_face_set(
      faces, corner_verts, vert_to_face_map, face_sets, subdiv_ccg, coord, allowed_face_sets);
}

bool coord_has_any_face_set(const OffsetIndices<int> faces,
                            const Span<int> corner_verts,
                            const GroupedSpan<int> vert_to_face_map,
                            const Span<int> face_sets,
                            const SubdivCCG &subdiv_ccg,
                            const SubdivCCGCoord coord,
                            const Set<int> &allowed_face_sets)
{
  if (face_sets.is_empty()) {
    return allowed_face_sets.contains(face_set_none_id);
  }

  if (allowed_face_sets.is_empty()) {
    return false;
  }

  int v1, v2;
  const SubdivCCGAdjacencyType adjacency = BKE_subdiv_ccg_coarse_mesh_adjacency_info_get(
      subdiv_ccg, coord, corner_verts, faces, v1, v2);
  switch (adjacency) {
    case SubdivCCGAdjacencyType::Vertex: {
      for (const int face : vert_to_face_map[v1]) {
        if (allowed_face_sets.contains(face_sets[face])) {
          return true;
        }
      }
      return false;
    }
    case SubdivCCGAdjacencyType::Edge:
      for (const int face : vert_to_face_map[v1]) {
        const Span<int> face_verts = corner_verts.slice(faces[face]);
        if (!face_verts.contains(v2)) {
          continue;
        }
        if (allowed_face_sets.contains(face_sets[face])) {
          return true;
        }
      }
      return false;
    case SubdivCCGAdjacencyType::None: {
      const int face = BKE_subdiv_ccg_grid_to_face_index(subdiv_ccg, coord.grid_index);
      return allowed_face_sets.contains(face_sets[face]);
    }
  }

  BLI_assert_unreachable();
  return false;
}

bool vert_has_unique_face_set(const int /*face_set_offset*/, const BMVert & /*vert*/)
{
  /* TODO: Obviously not fully implemented yet. Needs to be implemented for Relax Face Sets brush
   * to work. */
  return true;
}

}  // namespace face_set

Span<BMVert *> vert_neighbors_get_bmesh(BMVert &vert, BMeshNeighborVerts &r_neighbors)
{
  r_neighbors.clear();
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, &vert, BM_LOOPS_OF_VERT) {
    for (BMVert *other_vert : {l->prev->v, l->next->v}) {
      if (other_vert != &vert) {
        r_neighbors.append(other_vert);
      }
    }
  }
  return r_neighbors;
}

Span<BMVert *> vert_neighbors_get_interior_bmesh(BMVert &vert, BMeshNeighborVerts &r_neighbors)
{
  r_neighbors.clear();
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, &vert, BM_LOOPS_OF_VERT) {
    for (BMVert *other_vert : {l->prev->v, l->next->v}) {
      if (other_vert != &vert) {
        r_neighbors.append(other_vert);
      }
    }
  }

  if (BM_vert_is_boundary(&vert)) {
    if (r_neighbors.size() == 2) {
      /* Do not include neighbors of corner vertices. */
      r_neighbors.clear();
    }
    else {
      /* Only include other boundary vertices as neighbors of boundary vertices. */
      r_neighbors.remove_if([&](const BMVert *neighbor) {
        return !BM_edge_is_boundary(BM_edge_exists(&vert, const_cast<BMVert *>(neighbor)));
      });
    }
  }

  return r_neighbors;
}

Span<int> vert_neighbors_get_mesh(const OffsetIndices<int> faces,
                                  const Span<int> corner_verts,
                                  const GroupedSpan<int> vert_to_face,
                                  const Span<bool> hide_poly,
                                  const int vert,
                                  Vector<int> &r_neighbors)
{
  r_neighbors.clear();

  for (const int face : vert_to_face[vert]) {
    if (!hide_poly.is_empty() && hide_poly[face]) {
      continue;
    }
    const int2 verts = bke::mesh::face_find_adjacent_verts(faces[face], corner_verts, vert);
    r_neighbors.append_non_duplicates(verts[0]);
    r_neighbors.append_non_duplicates(verts[1]);
  }

  return r_neighbors.as_span();
}

inline void append_neighbors_to_vector(const OffsetIndices<int> faces,
                                       const Span<int> corner_verts,
                                       const GroupedSpan<int> vert_to_face,
                                       const Span<bool> hide_poly,
                                       const int vert,
                                       Vector<int> &r_data)
{
  const int vert_start = r_data.size();
  for (const int face : vert_to_face[vert]) {
    if (!hide_poly.is_empty() && hide_poly[face]) {
      continue;
    }
    /* In order to support non-manifold topology, both neighboring vertices are added for each
     * face corner. That results in half being duplicates for any "normal" topology. */
    const int2 neighbors = bke::mesh::face_find_adjacent_verts(faces[face], corner_verts, vert);
    for (const int neighbor : {neighbors[0], neighbors[1]}) {
      bool found = false;
      for (int i = r_data.size() - 1; i >= vert_start; i--) {
        if (r_data[i] == neighbor) {
          found = true;
          break;
        }
      }
      if (found) {
        continue;
      }
      r_data.append(neighbor);
    }
  }
}

namespace boundary {

void ensure_boundary_info(Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  if (ss.boundary_info_cache) {
    return;
  }

  ss.boundary_info_cache = std::make_unique<SculptBoundaryInfoCache>(
      create_boundary_info(*BKE_mesh_from_object(&object)));
}

SculptBoundaryInfoCache create_boundary_info(const Mesh &mesh)
{
  PRF_scope(ProfileCategory::Editor);
  SculptBoundaryInfoCache boundary_info;
  boundary_info.verts.resize(mesh.verts_num);
  Array<int> adjacent_faces_edge_count(mesh.edges_num, 0);
  array_utils::count_indices(mesh.corner_edges(), adjacent_faces_edge_count);

  const Span<int2> edges = mesh.edges();
  for (const int e : edges.index_range()) {
    if (adjacent_faces_edge_count[e] < 2) {
      const int2 &edge = edges[e];
      boundary_info.edges.add(edge);
      boundary_info.verts[edge[0]].set();
      boundary_info.verts[edge[1]].set();
    }
  }

  return boundary_info;
}

bool vert_is_boundary(const GroupedSpan<int> vert_to_face_map,
                      const Span<bool> hide_poly,
                      const BitSpan boundary_verts,
                      const int vert)
{
  if (!hide::vert_all_faces_visible_get(hide_poly, vert_to_face_map, vert)) {
    return true;
  }
  return boundary_verts[vert].test();
}

bool vert_is_boundary(const OffsetIndices<int> faces,
                      const Span<int> corner_verts,
                      const BitSpan boundary_verts,
                      const Set<OrderedEdge> &boundary_edges,
                      const SubdivCCG &subdiv_ccg,
                      const SubdivCCGCoord vert)
{
  /* TODO: Unlike the base mesh implementation this method does NOT take into account face
   * visibility. Either this should be noted as a intentional limitation or fixed. */
  return BKE_subdiv_ccg_coord_is_mesh_boundary(
      faces, corner_verts, boundary_verts, boundary_edges, subdiv_ccg, vert);
}

bool vert_is_boundary(BMVert *vert)
{
  /* TODO: Unlike the base mesh implementation this method does NOT take into account face
   * visibility. Either this should be noted as a intentional limitation or fixed. */
  return BM_vert_is_boundary(vert);
}

}  // namespace boundary

/* Utilities */

bool stroke_is_main_symmetry_pass(const StrokeCache &cache)
{
  return cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0 &&
         cache.tile_pass == 0;
}

bool stroke_is_first_brush_step(const StrokeCache &cache)
{
  return cache.first_time && cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0 &&
         cache.tile_pass == 0;
}

bool stroke_is_first_brush_step_of_symmetry_pass(const StrokeCache &cache)
{
  return cache.first_time;
}

bool check_vertex_pivot_symmetry(const float vco[3], const float pco[3], const char symm)
{
  bool is_in_symmetry_area = true;
  for (int i = 0; i < 3; i++) {
    char symm_it = 1 << i;
    if (symm & symm_it) {
      if (pco[i] == 0.0f) {
        if (vco[i] > 0.0f) {
          is_in_symmetry_area = false;
        }
      }
      if (vco[i] * pco[i] < 0.0f) {
        is_in_symmetry_area = false;
      }
    }
  }
  return is_in_symmetry_area;
}

/**
 * Align the grab delta to the brush normal.
 *
 * \param grab_delta: Typically from `ss.cache->grab_delta_symmetry`.
 */
static void sculpt_project_v3_normal_align(const StrokeCache &cache,
                                           const float normal_weight,
                                           float grab_delta[3])
{
  /* Signed to support grabbing in (to make a hole) as well as out. */
  const float len_signed = dot_v3v3(cache.sculpt_normal_symm, grab_delta);

  /* This scale effectively projects the offset so dragging follows the cursor,
   * as the normal points towards the view, the scale increases. */
  float len_view_scale;
  {
    float view_aligned_normal[3];
    project_plane_v3_v3v3(view_aligned_normal, cache.sculpt_normal_symm, cache.view_normal_symm);
    len_view_scale = fabsf(dot_v3v3(view_aligned_normal, cache.sculpt_normal_symm));
    len_view_scale = (len_view_scale > FLT_EPSILON) ? 1.0f / len_view_scale : 1.0f;
  }

  mul_v3_fl(grab_delta, 1.0f - normal_weight);
  madd_v3_v3fl(
      grab_delta, cache.sculpt_normal_symm, (len_signed * normal_weight) * len_view_scale);
}

float3 grab_delta_get(const Brush &brush, const StrokeCache &cache)
{
  float3 grab_delta = cache.grab_delta_symm;

  const float normal_weight = bke::brush::normal_weight_get(brush, cache.toggle_settings.invert);
  if (normal_weight > 0.0f) {
    sculpt_project_v3_normal_align(cache, normal_weight, grab_delta);
  }

  return grab_delta;
}

std::optional<int> nearest_vert_calc_mesh(const bke::pbvh::Tree &pbvh,
                                          const Span<float3> vert_positions,
                                          const Span<bool> hide_vert,
                                          const float3 &location,
                                          const float max_distance,
                                          const bool use_original)
{
  const float max_distance_sq = max_distance * max_distance;
  IndexMaskMemory memory;
  const IndexMask nodes_in_sphere = bke::pbvh::search_nodes(
      pbvh, memory, [&](const bke::pbvh::Node &node) {
        return node_in_sphere(node, location, max_distance_sq, use_original);
      });
  if (nodes_in_sphere.is_empty()) {
    return std::nullopt;
  }

  struct NearestData {
    int vert = -1;
    float distance_sq = std::numeric_limits<float>::max();
  };

  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const NearestData nearest = threading::parallel_reduce(
      nodes_in_sphere.index_range(),
      1,
      NearestData(),
      [&](const IndexRange range, NearestData nearest) {
        nodes_in_sphere.slice(range).foreach_index([&](const int i) {
          for (const int vert : nodes[i].verts()) {
            if (!hide_vert.is_empty() && hide_vert[vert]) {
              continue;
            }
            const float distance_sq = math::distance_squared(vert_positions[vert], location);
            if (distance_sq < nearest.distance_sq) {
              nearest = {vert, distance_sq};
            }
          }
        });
        return nearest;
      },
      [](const NearestData a, const NearestData b) {
        return a.distance_sq < b.distance_sq ? a : b;
      });
  return nearest.vert;
}

std::optional<SubdivCCGCoord> nearest_vert_calc_grids(const bke::pbvh::Tree &pbvh,
                                                      const SubdivCCG &subdiv_ccg,
                                                      const float3 &location,
                                                      const float max_distance,
                                                      const bool use_original)
{
  const float max_distance_sq = max_distance * max_distance;
  IndexMaskMemory memory;
  const IndexMask nodes_in_sphere = bke::pbvh::search_nodes(
      pbvh, memory, [&](const bke::pbvh::Node &node) {
        return node_in_sphere(node, location, max_distance_sq, use_original);
      });
  if (nodes_in_sphere.is_empty()) {
    return std::nullopt;
  }

  struct NearestData {
    SubdivCCGCoord coord = {};
    float distance_sq = std::numeric_limits<float>::max();
  };

  const BitGroupVector<> grid_hidden = subdiv_ccg.grid_hidden;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float3> positions = subdiv_ccg.positions;

  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const NearestData nearest = threading::parallel_reduce(
      nodes_in_sphere.index_range(),
      1,
      NearestData(),
      [&](const IndexRange range, NearestData nearest) {
        nodes_in_sphere.slice(range).foreach_index([&](const int i) {
          for (const int grid : nodes[i].grids()) {
            const IndexRange grid_range = bke::ccg::grid_range(key, grid);
            BKE_subdiv_ccg_foreach_visible_grid_vert(key, grid_hidden, grid, [&](const int i) {
              const float distance_sq = math::distance_squared(positions[grid_range[i]], location);
              if (distance_sq < nearest.distance_sq) {
                SubdivCCGCoord coord{};
                coord.grid_index = grid;
                coord.x = i % key.grid_size;
                coord.y = i / key.grid_size;
                nearest = {coord, distance_sq};
              }
            });
          }
        });
        return nearest;
      },
      [](const NearestData a, const NearestData b) {
        return a.distance_sq < b.distance_sq ? a : b;
      });
  return nearest.coord;
}

std::optional<BMVert *> nearest_vert_calc_bmesh(const bke::pbvh::Tree &pbvh,
                                                const float3 &location,
                                                const float max_distance,
                                                const bool use_original)
{
  const float max_distance_sq = max_distance * max_distance;
  IndexMaskMemory memory;
  const IndexMask nodes_in_sphere = bke::pbvh::search_nodes(
      pbvh, memory, [&](const bke::pbvh::Node &node) {
        return node_in_sphere(node, location, max_distance_sq, use_original);
      });
  if (nodes_in_sphere.is_empty()) {
    return std::nullopt;
  }

  struct NearestData {
    BMVert *vert = nullptr;
    float distance_sq = std::numeric_limits<float>::max();
  };

  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const NearestData nearest = threading::parallel_reduce(
      nodes_in_sphere.index_range(),
      1,
      NearestData(),
      [&](const IndexRange range, NearestData nearest) {
        nodes_in_sphere.slice(range).foreach_index([&](const int i) {
          for (BMVert *vert :
               BKE_pbvh_bmesh_node_unique_verts(const_cast<bke::pbvh::BMeshNode *>(&nodes[i])))
          {
            if (BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
              continue;
            }
            const float distance_sq = math::distance_squared(float3(vert->co), location);
            if (distance_sq < nearest.distance_sq) {
              nearest = {vert, distance_sq};
            }
          }
        });
        return nearest;
      },
      [](const NearestData a, const NearestData b) {
        return a.distance_sq < b.distance_sq ? a : b;
      });
  return nearest.vert;
}

bool is_vertex_inside_brush_radius_symm(const float vertex[3],
                                        const float br_co[3],
                                        float radius,
                                        char symm)
{
  for (char i = 0; i <= symm; ++i) {
    if (!is_symmetry_iteration_valid(i, symm)) {
      continue;
    }
    float3 location = symmetry_flip(br_co, ePaintSymmetryFlags(i));
    if (len_squared_v3v3(location, vertex) < radius * radius) {
      return true;
    }
  }
  return false;
}

void tag_update_overlays(bContext *C)
{
  ARegion *region = CTX_wm_region(C);
  ED_region_tag_redraw(region);

  Object &ob = *CTX_data_active_object(C);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);

  DEG_id_tag_update(&ob.id, ID_RECALC_SHADING);

  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (!BKE_sculptsession_use_pbvh_draw(&ob, rv3d)) {
    DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Capabilities
 *
 * Avoid duplicate checks, internal logic only,
 * share logic with #rna_def_sculpt_capabilities where possible.
 * \{ */

static bool brush_type_needs_original(const char sculpt_brush_type)
{
  return ELEM(sculpt_brush_type,
              SCULPT_BRUSH_TYPE_GRAB,
              SCULPT_BRUSH_TYPE_ROTATE,
              SCULPT_BRUSH_TYPE_THUMB,
              SCULPT_BRUSH_TYPE_LAYER,
              SCULPT_BRUSH_TYPE_DRAW_SHARP,
              SCULPT_BRUSH_TYPE_ELASTIC_DEFORM,
              SCULPT_BRUSH_TYPE_SMOOTH,
              SCULPT_BRUSH_TYPE_BOUNDARY,
              SCULPT_BRUSH_TYPE_POSE);
}

static bool brush_uses_topology_rake(const SculptSession &ss, const Brush &brush)
{
  return bke::brush::supports_topology_rake(brush) && (brush.topology_rake_factor > 0.0f) &&
         (ss.bm != nullptr);
}

/**
 * Test whether the #StrokeCache.sculpt_normal needs update in #do_brush_action
 */
static int sculpt_brush_needs_normal(const SculptSession &ss, const Brush &brush)
{
  const MTex *mask_tex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  return ((bke::brush::supports_normal_weight(brush) &&
           (bke::brush::normal_weight_get(brush, ss.cache->toggle_settings.invert) > 0.0f)) ||
          ELEM(brush.sculpt_brush_type,
               SCULPT_BRUSH_TYPE_BLOB,
               SCULPT_BRUSH_TYPE_CREASE,
               SCULPT_BRUSH_TYPE_DRAW,
               SCULPT_BRUSH_TYPE_DRAW_SHARP,
               SCULPT_BRUSH_TYPE_CLOTH,
               SCULPT_BRUSH_TYPE_LAYER,
               SCULPT_BRUSH_TYPE_NUDGE,
               SCULPT_BRUSH_TYPE_ROTATE,
               SCULPT_BRUSH_TYPE_ELASTIC_DEFORM,
               SCULPT_BRUSH_TYPE_THUMB) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SCENE_PROJECT &&
           brush.project_ray_direction_type == BRUSH_PROJECT_RAY_DIRECTION_PLANE_NORMAL) ||
          (mask_tex->tex && mask_tex->brush_map_mode == MTEX_MAP_MODE_AREA)) ||
         brush_uses_topology_rake(ss, brush) || BKE_brush_has_cube_tip(&brush, PaintMode::Sculpt);
}

static bool brush_needs_rake_rotation(const Brush &brush)
{
  return bke::brush::supports_rake_factor(brush) && (brush.rake_factor != 0.0f);
}

/** \} */

static void rake_data_update(SculptRakeData *srd, const float co[3])
{
  float rake_dist = len_v3v3(srd->follow_co, co);
  if (rake_dist > srd->follow_dist) {
    interp_v3_v3v3(srd->follow_co, srd->follow_co, co, rake_dist - srd->follow_dist);
  }
}

/* -------------------------------------------------------------------- */
/** \name Sculpt Dynamic Topology
 * \{ */

namespace dyntopo {

bool stroke_is_dyntopo(const Object &object, const Brush &brush)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  return ((pbvh.type() == bke::pbvh::Type::BMesh) &&
          (!ss.cache || (!ss.cache->toggle_settings.alt_smooth)) &&
          /* Requires mesh restore, which doesn't work with
           * dynamic-topology. */
          !(ELEM(brush.stroke_method, BRUSH_STROKE_ANCHORED, BRUSH_STROKE_DRAG_DOT)) &&
          bke::brush::supports_dyntopo(brush));
}

}  // namespace dyntopo

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Paint Mesh
 * \{ */

namespace undo {

static void restore_mask_from_undo_step(Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  Array<bool> node_changed(node_mask.min_array_size(), false);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
          ".sculpt_mask", bke::AttrDomain::Point);
      node_mask.foreach_index(
          [&](const int i) {
            if (const std::optional<Span<float>> orig_data = orig_mask_data_lookup_mesh(object,
                                                                                        nodes[i]))
            {
              const Span<int> verts = nodes[i].verts();
              scatter_data_mesh(*orig_data, verts, mask.span);
              bke::pbvh::node_update_mask_mesh(mask.span, nodes[i]);
              node_changed[i] = true;
            }
          },
          exec_mode::grain_size(1));
      mask.finish();
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      const int offset = CustomData_get_offset_named(&ss.bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
      if (offset != -1) {
        node_mask.foreach_index(
            [&](const int i) {
              for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(&nodes[i])) {
                if (const float *orig_mask = BM_log_find_original_vert_mask(ss.bm_log, vert)) {
                  BM_ELEM_CD_SET_FLOAT(vert, offset, *orig_mask);
                  bke::pbvh::node_update_mask_bmesh(offset, nodes[i]);
                  node_changed[i] = true;
                }
              }
            },
            exec_mode::grain_size(1));
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const BitGroupVector<> grid_hidden = subdiv_ccg.grid_hidden;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      MutableSpan<float> masks = subdiv_ccg.masks;
      node_mask.foreach_index(
          [&](const int i) {
            if (const std::optional<Span<float>> orig_data = orig_mask_data_lookup_grids(object,
                                                                                         nodes[i]))
            {
              int index = 0;
              for (const int grid : nodes[i].grids()) {
                const IndexRange grid_range = bke::ccg::grid_range(key, grid);
                for (const int i : IndexRange(key.grid_area)) {
                  if (grid_hidden.is_empty() || !grid_hidden[grid][i]) {
                    masks[grid_range[i]] = (*orig_data)[index];
                  }
                  index++;
                }
              }
              bke::pbvh::node_update_mask_grids(key, masks, nodes[i]);
              node_changed[i] = true;
            }
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  pbvh.tag_masks_changed(IndexMask::from_bools(node_changed, memory));
}

static void restore_color_from_undo_step(Object &object)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  /* The experimental grid painter currently accumulates local color deltas.
   * This mesh-only dab reset cannot reinterpret Grid nodes as Mesh nodes.
   * Full stroke Undo/Redo uses the separate master-color snapshot in sculpt_undo.cc. */
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return;
  }
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_predicate(
      nodes.index_range(),
      memory,
      [&](const int i) { return orig_color_data_lookup_mesh(object, nodes[i]).has_value(); },
      exec_mode::grain_size(64));

  BLI_assert(pbvh.type() == bke::pbvh::Type::Mesh);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  bke::GSpanAttributeWriter color_attribute = color::active_color_attribute_for_write(mesh);
  node_mask.foreach_index(
      [&](const int i) {
        const Span<float4> orig_data = *orig_color_data_lookup_mesh(object, nodes[i]);
        const Span<int> verts = nodes[i].verts();
        for (const int i : verts.index_range()) {
          color::color_vert_set(faces,
                                corner_verts,
                                vert_to_face_map,
                                color_attribute.domain,
                                verts[i],
                                orig_data[i],
                                color_attribute.span);
        }
      },
      exec_mode::grain_size(1));
  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

static void restore_face_set_from_undo_step(Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  Array<bool> node_changed(node_mask.min_array_size(), false);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      bke::SpanAttributeWriter<int> attribute = face_set::ensure_face_sets_mesh(
          *id_cast<Mesh *>(object.data));
      node_mask.foreach_index(
          [&](const int i) {
            if (const std::optional<Span<int>> orig_data = orig_face_set_data_lookup_mesh(
                    object, nodes[i]))
            {
              scatter_data_mesh(*orig_data, nodes[i].faces(), attribute.span);
              node_changed[i] = true;
            }
          },
          exec_mode::grain_size(1));
      attribute.finish();
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      bke::SpanAttributeWriter<int> attribute = face_set::ensure_face_sets_mesh(
          *id_cast<Mesh *>(object.data));
      threading::EnumerableThreadSpecific<Vector<int>> all_tls;
      node_mask.foreach_index(
          [&](const int i) {
            Vector<int> &tls = all_tls.local();
            if (const std::optional<Span<int>> orig_data = orig_face_set_data_lookup_grids(
                    object, nodes[i]))
            {
              const Span<int> faces = bke::pbvh::node_face_indices_calc_grids(
                  subdiv_ccg, nodes[i], tls);
              scatter_data_mesh(*orig_data, faces, attribute.span);
              node_changed[i] = true;
            }
          },
          exec_mode::grain_size(1));
      attribute.finish();
      break;
    }
    case bke::pbvh::Type::BMesh:
      break;
  }

  pbvh.tag_face_sets_changed(IndexMask::from_bools(node_changed, memory));
}

void restore_position_from_undo_step(const Depsgraph &depsgraph, Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  IndexMaskMemory memory;

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      MutableSpan positions_eval = bke::pbvh::vert_positions_eval_for_write(depsgraph, object);
      MutableSpan positions_orig = mesh.vert_positions_for_write();

      const IndexMask node_mask = IndexMask::from_predicate(
          nodes.index_range(),
          memory,
          [&](const int i) {
            return orig_position_data_lookup_mesh(object, nodes[i]).has_value();
          },
          exec_mode::grain_size(64));

      struct LocalData {
        Vector<float3> translations;
      };

      MultiresSculptLayer *base_layer = bke::multires_layers::base_recording_layer(object);
      std::optional<ShapeKeyData> shape_key_data = ShapeKeyData::from_object(object);
      const bool need_translations = base_layer || !ss.deform_imats.is_empty() || shape_key_data.has_value();

      threading::EnumerableThreadSpecific<LocalData> all_tls;
      node_mask.foreach_index(
          [&](const int i) {
            threading::isolate_task([&] {
              LocalData &tls = all_tls.local();
              const OrigPositionData orig_data = *orig_position_data_lookup_mesh(object, nodes[i]);
              const Span<int> verts = nodes[i].verts();
              const Span<float3> undo_positions = orig_data.positions;
              if (need_translations) {
                /* Calculate translations from evaluated positions before they are changed. */
                tls.translations.resize(verts.size());
                translations_from_new_positions(
                    undo_positions, verts, positions_eval, tls.translations);
              }

              scatter_data_mesh(undo_positions, verts, positions_eval);

              if (positions_eval.data() == positions_orig.data()) {
                return;
              }

              const MutableSpan<float3> translations = tls.translations;
              if (!ss.deform_imats.is_empty()) {
                apply_crazyspace_to_translations(ss.deform_imats, verts, translations);
              }

              if (base_layer) {
                bke::multires_layers::base_deltas_record(*base_layer, verts, translations);
                return;
              }
              if (shape_key_data) {
                for (MutableSpan<float3> data : shape_key_data->dependent_keys) {
                  apply_translations(translations, verts, data);
                }

                if (shape_key_data->basis_key_active) {
                  /* The basis key positions and the mesh positions are always kept in sync. */
                  apply_translations(translations, verts, positions_orig);
                }
                apply_translations(translations, verts, shape_key_data->active_key_data);
              }
              else {
                apply_translations(translations, verts, positions_orig);
              }
            });
          },
          exec_mode::grain_size(1));
      pbvh.tag_positions_changed(node_mask);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      if (!undo::has_bmesh_log_entry()) {
        return;
      }
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
      node_mask.foreach_index(
          [&](const int i) {
            for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(&nodes[i])) {
              if (const float *orig_co = BM_log_find_original_vert_co(ss.bm_log, vert)) {
                copy_v3_v3(vert->co, orig_co);
              }
            }
          },
          exec_mode::grain_size(1));
      pbvh.tag_positions_changed(node_mask);
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

      const IndexMask node_mask = IndexMask::from_predicate(
          nodes.index_range(),
          memory,
          [&](const int i) {
            return orig_position_data_lookup_grids(object, nodes[i]).has_value();
          },
          exec_mode::grain_size(64));

      SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const BitGroupVector<> grid_hidden = subdiv_ccg.grid_hidden;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      MutableSpan<float3> positions = subdiv_ccg.positions;
      node_mask.foreach_index(
          [&](const int i) {
            const OrigPositionData orig_data = *orig_position_data_lookup_grids(object, nodes[i]);
            int index = 0;
            for (const int grid : nodes[i].grids()) {
              const IndexRange grid_range = bke::ccg::grid_range(key, grid);
              for (const int i : IndexRange(key.grid_area)) {
                if (grid_hidden.is_empty() || !grid_hidden[grid][i]) {
                  positions[grid_range[i]] = orig_data.positions[index];
                }
                index++;
              }
            }
          },
          exec_mode::grain_size(1));
      pbvh.tag_positions_changed(node_mask);
      break;
    }
  }
}

static void restore_from_undo_step(const Depsgraph &depsgraph, const Sculpt &sd, Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  switch (brush->sculpt_brush_type) {
    case SCULPT_BRUSH_TYPE_MASK:
      restore_mask_from_undo_step(object);
      break;
    case SCULPT_BRUSH_TYPE_BLUR:
    case SCULPT_BRUSH_TYPE_PAINT:
    case SCULPT_BRUSH_TYPE_SMEAR:
      restore_color_from_undo_step(object);
      break;
    case SCULPT_BRUSH_TYPE_DRAW_FACE_SETS:
      if (ss.cache->toggle_settings.alt_smooth) {
        restore_position_from_undo_step(depsgraph, object);
        bke::pbvh::update_normals(depsgraph, object, *bke::object::pbvh_get(object));
      }
      else {
        restore_face_set_from_undo_step(object);
      }
      break;
    default:
      restore_position_from_undo_step(depsgraph, object);
      bke::pbvh::update_normals(depsgraph, object, *bke::object::pbvh_get(object));
      break;
  }
}

}  // namespace undo

const float *brush_frontface_normal_from_falloff_shape(const SculptSession &ss, char falloff_shape)
{
  if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
    return ss.cache->sculpt_normal_symm;
  }
  BLI_assert(falloff_shape == PAINT_FALLOFF_SHAPE_TUBE);
  return ss.cache->view_normal_symm;
}

/* ===== Sculpting =====
 */

static float calc_overlap(const StrokeCache &cache,
                          const ePaintSymmetryFlags symm,
                          const char axis,
                          const float angle)
{
  float3 mirror = symmetry_flip(cache.location, symm);

  if (axis != 0) {
    float mat[3][3];
    axis_angle_to_mat3_single(mat, axis, angle);
    mul_m3_v3(mat, mirror);
  }

  const float distsq = len_squared_v3v3(mirror, cache.location);

  if (distsq <= 4.0f * (cache.radius_squared)) {
    return (2.0f * (cache.radius) - sqrtf(distsq)) / (2.0f * (cache.radius));
  }
  return 0.0f;
}

static float calc_radial_symmetry_feather(const Mesh &mesh,
                                          const StrokeCache &cache,
                                          const ePaintSymmetryFlags symm,
                                          const char axis)
{
  float overlap = 0.0f;

  for (int i = 1; i < mesh.radial_symmetry[axis - 'X']; i++) {
    const float angle = 2.0f * M_PI * i / mesh.radial_symmetry[axis - 'X'];
    overlap += calc_overlap(cache, symm, axis, angle);
  }

  return overlap;
}

static float calc_symmetry_feather(const Sculpt &sd,
                                   const ePaintSymmetryFlags symm,
                                   const Mesh &mesh,
                                   const StrokeCache &cache)
{
  if (!(sd.paint.symmetry_flags & PAINT_SYMMETRY_FEATHER)) {
    return 1.0f;
  }
  float overlap;

  overlap = 0.0f;
  for (int i = 0; i <= symm; i++) {
    if (!is_symmetry_iteration_valid(i, symm)) {
      continue;
    }

    overlap += calc_overlap(cache, ePaintSymmetryFlags(i), 0, 0);

    overlap += calc_radial_symmetry_feather(mesh, cache, ePaintSymmetryFlags(i), 'X');
    overlap += calc_radial_symmetry_feather(mesh, cache, ePaintSymmetryFlags(i), 'Y');
    overlap += calc_radial_symmetry_feather(mesh, cache, ePaintSymmetryFlags(i), 'Z');
  }
  return 1.0f / overlap;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Calculate Normal and Center
 *
 * Calculate geometry surrounding the brush center.
 * (optionally using original coordinates).
 *
 * Functions are:
 * - #calc_area_center
 * - #calc_area_normal
 * - #calc_area_normal_and_center
 *
 * \note These are all _very_ similar, when changing one, check others.
 * \{ */

struct AreaNormalCenterData {
  /* 0 = towards view, 1 = flipped */
  std::array<float3, 2> area_cos;
  std::array<int, 2> count_co;

  std::array<float3, 2> area_nos;
  std::array<int, 2> count_no;
};

static float area_normal_and_center_get_normal_radius(const SculptSession &ss, const Brush &brush)
{
  float test_radius = ss.cache ? ss.cache->radius : ss.cursor_radius;
  if (brush.ob_mode == OB_MODE_SCULPT) {
    test_radius *= brush.normal_radius_factor;
  }
  return test_radius;
}

static float area_normal_and_center_get_position_radius(const SculptSession &ss,
                                                        const Brush &brush)
{
  float test_radius = ss.cache ? ss.cache->radius : ss.cursor_radius;
  if (brush.ob_mode == OB_MODE_SCULPT) {
    /* Layer brush produces artifacts with normal and area radius */
    if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_PLANE && brush.area_radius_factor > 0.0f) {
      test_radius *= brush.area_radius_factor;
      if (ss.cache && brush.flag2 & BRUSH_AREA_RADIUS_PRESSURE) {
        test_radius *= ss.cache->pressure;
      }
    }
    else {
      test_radius *= brush.normal_radius_factor;
    }
  }
  return test_radius;
}

/* Weight the normals towards the center. */
static float area_normal_calc_weight(const float distance, const float radius_inv)
{
  float p = 1.0f - (distance * radius_inv);
  return std::clamp(3.0f * p * p - 2.0f * p * p * p, 0.0f, 1.0f);
}

/* Weight the coordinates towards the center. */
static float3 area_center_calc_weighted(const float3 &test_location,
                                        const float distance,
                                        const float radius_inv,
                                        const float3 &co)
{
  /* Weight the coordinates towards the center. */
  float p = 1.0f - (distance * radius_inv);
  const float afactor = std::clamp(3.0f * p * p - 2.0f * p * p * p, 0.0f, 1.0f);

  const float3 disp = (co - test_location) * (1.0f - afactor);
  return test_location + disp;
}

static void accumulate_area_center(const float3 &test_location,
                                   const float3 &position,
                                   const float distance,
                                   const float radius_inv,
                                   const int flip_index,
                                   AreaNormalCenterData &anctd)
{
  anctd.area_cos[flip_index] += area_center_calc_weighted(
      test_location, distance, radius_inv, position);
  anctd.count_co[flip_index] += 1;
}

static void accumulate_area_normal(const float3 &normal,
                                   const float distance,
                                   const float radius_inv,
                                   const int flip_index,
                                   AreaNormalCenterData &anctd)
{
  anctd.area_nos[flip_index] += normal * area_normal_calc_weight(distance, radius_inv);
  anctd.count_no[flip_index] += 1;
}

struct SampleLocalData {
  Vector<float3> positions;
  Vector<float> distances;
};

enum class AverageDataFlags : uint8_t {
  Position = 1 << 0,
  Normal = 1 << 1,

  All = Position | Normal
};
ENUM_OPERATORS(AverageDataFlags);

static void calc_area_normal_and_center_node_mesh(const Object &object,
                                                  const Span<float3> vert_positions,
                                                  const Span<float3> vert_normals,
                                                  const Span<bool> hide_vert,
                                                  const Brush &brush,
                                                  const AverageDataFlags flag,
                                                  const bke::pbvh::MeshNode &node,
                                                  SampleLocalData &tls,
                                                  AreaNormalCenterData &anctd)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const float3 &location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  const float3 &view_normal = ss.cache ? ss.cache->view_normal_symm : ss.cursor_view_normal;
  const float position_radius = area_normal_and_center_get_position_radius(ss, brush);
  const float position_radius_sq = position_radius * position_radius;
  const float position_radius_inv = math::rcp(position_radius);
  const float normal_radius = area_normal_and_center_get_normal_radius(ss, brush);
  const float normal_radius_sq = normal_radius * normal_radius;
  const float normal_radius_inv = math::rcp(normal_radius);

  const Span<int> verts = node.verts();

  if (ss.cache && !ss.cache->accum) {
    if (const std::optional<OrigPositionData> orig_data = orig_position_data_lookup_mesh(object,
                                                                                         node))
    {
      const Span<float3> orig_positions = orig_data->positions;
      const Span<float3> orig_normals = orig_data->normals;

      tls.distances.reinitialize(verts.size());
      const MutableSpan<float> distances_sq = tls.distances;
      calc_brush_distances_squared(
          ss, orig_positions, eBrushFalloffShape(brush.falloff_shape), distances_sq);

      for (const int i : verts.index_range()) {
        const int vert = verts[i];
        if (!hide_vert.is_empty() && hide_vert[vert]) {
          continue;
        }
        const bool needs_normal = flag_is_set(flag, AverageDataFlags::Normal) &&
                                  distances_sq[i] <= normal_radius_sq;
        const bool needs_center = flag_is_set(flag, AverageDataFlags::Position) &&
                                  distances_sq[i] <= position_radius_sq;
        if (!needs_normal && !needs_center) {
          continue;
        }
        const float3 &normal = orig_normals[i];
        const float distance = std::sqrt(distances_sq[i]);
        const int flip_index = math::dot(view_normal, normal) <= 0.0f;
        if (needs_center) {
          accumulate_area_center(
              location, orig_positions[i], distance, position_radius_inv, flip_index, anctd);
        }
        if (needs_normal) {
          accumulate_area_normal(normal, distance, normal_radius_inv, flip_index, anctd);
        }
      }
      return;
    }
  }

  tls.distances.reinitialize(verts.size());
  const MutableSpan<float> distances_sq = tls.distances;
  calc_brush_distances_squared(
      ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances_sq);

  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    if (!hide_vert.is_empty() && hide_vert[vert]) {
      continue;
    }
    const bool needs_normal = flag_is_set(flag, AverageDataFlags::Normal) &&
                              distances_sq[i] <= normal_radius_sq;
    const bool needs_center = flag_is_set(flag, AverageDataFlags::Position) &&
                              distances_sq[i] <= position_radius_sq;
    if (!needs_normal && !needs_center) {
      continue;
    }
    const float3 &normal = vert_normals[vert];
    const float distance = std::sqrt(distances_sq[i]);
    const int flip_index = math::dot(view_normal, normal) <= 0.0f;
    if (needs_center) {
      accumulate_area_center(
          location, vert_positions[vert], distance, position_radius_inv, flip_index, anctd);
    }
    if (needs_normal) {
      accumulate_area_normal(normal, distance, normal_radius_inv, flip_index, anctd);
    }
  }
}

static void calc_area_normal_and_center_node_grids(const Object &object,
                                                   const Brush &brush,
                                                   const AverageDataFlags flag,
                                                   const bke::pbvh::GridsNode &node,
                                                   SampleLocalData &tls,
                                                   AreaNormalCenterData &anctd)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const float3 &location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  const float3 &view_normal = ss.cache ? ss.cache->view_normal_symm : ss.cursor_view_normal;
  const float position_radius = area_normal_and_center_get_position_radius(ss, brush);
  const float position_radius_sq = position_radius * position_radius;
  const float position_radius_inv = math::rcp(position_radius);
  const float normal_radius = area_normal_and_center_get_normal_radius(ss, brush);
  const float normal_radius_sq = normal_radius * normal_radius;
  const float normal_radius_inv = math::rcp(normal_radius);

  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
  const Span<float3> normals = subdiv_ccg.normals;
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  const Span<int> grids = node.grids();

  if (ss.cache && !ss.cache->accum) {
    if (const std::optional<OrigPositionData> orig_data = orig_position_data_lookup_grids(object,
                                                                                          node))
    {
      const Span<float3> orig_positions = orig_data->positions;
      const Span<float3> orig_normals = orig_data->normals;

      tls.distances.reinitialize(orig_positions.size());
      const MutableSpan<float> distances_sq = tls.distances;
      calc_brush_distances_squared(
          ss, orig_positions, eBrushFalloffShape(brush.falloff_shape), distances_sq);

      for (const int i : grids.index_range()) {
        const IndexRange grid_range_node = bke::ccg::grid_range(key, i);
        const int grid = grids[i];
        for (const int offset : IndexRange(key.grid_area)) {
          if (!grid_hidden.is_empty() && grid_hidden[grid][offset]) {
            continue;
          }
          const int node_vert = grid_range_node[offset];

          const bool needs_normal = flag_is_set(flag, AverageDataFlags::Normal) &&
                                    distances_sq[node_vert] <= normal_radius_sq;
          const bool needs_center = flag_is_set(flag, AverageDataFlags::Position) &&
                                    distances_sq[node_vert] <= position_radius_sq;
          if (!needs_normal && !needs_center) {
            continue;
          }
          const float3 &normal = orig_normals[node_vert];
          const float distance = std::sqrt(distances_sq[node_vert]);
          const int flip_index = math::dot(view_normal, normal) <= 0.0f;
          if (needs_center) {
            accumulate_area_center(location,
                                   orig_positions[node_vert],
                                   distance,
                                   position_radius_inv,
                                   flip_index,
                                   anctd);
          }
          if (needs_normal) {
            accumulate_area_normal(normal, distance, normal_radius_inv, flip_index, anctd);
          }
        }
      }
      return;
    }
  }

  const Span<float3> positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);
  tls.distances.reinitialize(positions.size());
  const MutableSpan<float> distances_sq = tls.distances;
  calc_brush_distances_squared(
      ss, positions, eBrushFalloffShape(brush.falloff_shape), distances_sq);

  for (const int i : grids.index_range()) {
    const IndexRange grid_range_node = bke::ccg::grid_range(key, i);
    const int grid = grids[i];
    const IndexRange grid_range = bke::ccg::grid_range(key, grid);
    for (const int offset : IndexRange(key.grid_area)) {
      if (!grid_hidden.is_empty() && grid_hidden[grid][offset]) {
        continue;
      }
      const int node_vert = grid_range_node[offset];
      const int vert = grid_range[offset];

      const bool needs_normal = flag_is_set(flag, AverageDataFlags::Normal) &&
                                distances_sq[node_vert] <= normal_radius_sq;
      const bool needs_center = flag_is_set(flag, AverageDataFlags::Position) &&
                                distances_sq[node_vert] <= position_radius_sq;
      if (!needs_normal && !needs_center) {
        continue;
      }
      const float3 &normal = normals[vert];
      const float distance = std::sqrt(distances_sq[node_vert]);
      const int flip_index = math::dot(view_normal, normal) <= 0.0f;
      if (needs_center) {
        accumulate_area_center(
            location, positions[node_vert], distance, position_radius_inv, flip_index, anctd);
      }
      if (needs_normal) {
        accumulate_area_normal(normal, distance, normal_radius_inv, flip_index, anctd);
      }
    }
  }
}

static void calc_area_normal_and_center_node_bmesh(const Object &object,
                                                   const Brush &brush,
                                                   const AverageDataFlags flag,
                                                   const bool has_bm_orco,
                                                   const bke::pbvh::BMeshNode &node,
                                                   SampleLocalData &tls,
                                                   AreaNormalCenterData &anctd)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const float3 &location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  const float3 &view_normal = ss.cache ? ss.cache->view_normal_symm : ss.cursor_view_normal;
  const float position_radius = area_normal_and_center_get_position_radius(ss, brush);
  const float position_radius_sq = position_radius * position_radius;
  const float position_radius_inv = math::rcp(position_radius);
  const float normal_radius = area_normal_and_center_get_normal_radius(ss, brush);
  const float normal_radius_sq = normal_radius * normal_radius;
  const float normal_radius_inv = math::rcp(normal_radius);

  bool use_original = false;
  if (ss.cache && !ss.cache->accum) {
    use_original = undo::has_bmesh_log_entry();
  }

  /* When the mesh is edited we can't rely on original coords
   * (original mesh may not even have verts in brush radius). */
  if (use_original && has_bm_orco) {
    Span<float3> orig_positions;
    Span<int3> orig_tris;
    BKE_pbvh_node_get_bm_orco_data(node, orig_positions, orig_tris);

    tls.positions.resize(orig_tris.size());
    const MutableSpan<float3> positions = tls.positions;
    for (const int i : orig_tris.index_range()) {
      const float *co_tri[3] = {
          orig_positions[orig_tris[i][0]],
          orig_positions[orig_tris[i][1]],
          orig_positions[orig_tris[i][2]],
      };
      closest_on_tri_to_point_v3(positions[i], location, UNPACK3(co_tri));
    }

    tls.distances.reinitialize(positions.size());
    const MutableSpan<float> distances_sq = tls.distances;
    calc_brush_distances_squared(
        ss, positions, eBrushFalloffShape(brush.falloff_shape), distances_sq);

    for (const int i : orig_tris.index_range()) {
      const bool needs_normal = flag_is_set(flag, AverageDataFlags::Normal) &&
                                distances_sq[i] <= normal_radius_sq;
      const bool needs_center = flag_is_set(flag, AverageDataFlags::Position) &&
                                distances_sq[i] <= position_radius_sq;
      if (!needs_normal && !needs_center) {
        continue;
      }
      const float3 normal = math::normal_tri(float3(orig_positions[orig_tris[i][0]]),
                                             float3(orig_positions[orig_tris[i][1]]),
                                             float3(orig_positions[orig_tris[i][2]]));

      const float distance = std::sqrt(distances_sq[i]);
      const int flip_index = math::dot(view_normal, normal) <= 0.0f;
      if (needs_center) {
        accumulate_area_center(
            location, positions[i], distance, position_radius_inv, flip_index, anctd);
      }
      if (needs_normal) {
        accumulate_area_normal(normal, distance, normal_radius_inv, flip_index, anctd);
      }
    }
    return;
  }

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(
      &const_cast<bke::pbvh::BMeshNode &>(node));
  if (use_original) {
    tls.positions.resize(verts.size());
    const MutableSpan<float3> positions = tls.positions;
    Array<float3> normals(verts.size());
    orig_position_data_gather_bmesh(*ss.bm_log, verts, positions, normals);

    tls.distances.reinitialize(positions.size());
    const MutableSpan<float> distances_sq = tls.distances;
    calc_brush_distances_squared(
        ss, positions, eBrushFalloffShape(brush.falloff_shape), distances_sq);

    int i = 0;
    for (BMVert *vert : verts) {
      if (BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
        i++;
        continue;
      }
      const bool needs_normal = flag_is_set(flag, AverageDataFlags::Normal) &&
                                distances_sq[i] <= normal_radius_sq;
      const bool needs_center = flag_is_set(flag, AverageDataFlags::Position) &&
                                distances_sq[i] <= position_radius_sq;
      if (!needs_normal && !needs_center) {
        i++;
        continue;
      }
      const float3 &normal = normals[i];
      const float distance = std::sqrt(distances_sq[i]);
      const int flip_index = math::dot(view_normal, normal) <= 0.0f;
      if (needs_center) {
        accumulate_area_center(
            location, positions[i], distance, position_radius_inv, flip_index, anctd);
      }
      if (needs_normal) {
        accumulate_area_normal(normal, distance, normal_radius_inv, flip_index, anctd);
      }
      i++;
    }
    return;
  }

  const Span<float3> positions = gather_bmesh_positions(verts, tls.positions);

  tls.distances.reinitialize(positions.size());
  const MutableSpan<float> distances_sq = tls.distances;
  calc_brush_distances_squared(
      ss, positions, eBrushFalloffShape(brush.falloff_shape), distances_sq);

  int i = 0;
  for (BMVert *vert : verts) {
    if (BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
      i++;
      continue;
    }
    const bool needs_normal = flag_is_set(flag, AverageDataFlags::Normal) &&
                              distances_sq[i] <= normal_radius_sq;
    const bool needs_center = flag_is_set(flag, AverageDataFlags::Position) &&
                              distances_sq[i] <= position_radius_sq;
    if (!needs_normal && !needs_center) {
      i++;
      continue;
    }
    const float3 normal = vert->no;
    const float distance = std::sqrt(distances_sq[i]);
    const int flip_index = math::dot(view_normal, normal) <= 0.0f;
    if (needs_center) {
      accumulate_area_center(
          location, positions[i], distance, position_radius_inv, flip_index, anctd);
    }
    if (needs_normal) {
      accumulate_area_normal(normal, distance, normal_radius_inv, flip_index, anctd);
    }
    i++;
  }
}

static AreaNormalCenterData calc_area_normal_and_center_reduce(const AreaNormalCenterData &a,
                                                               const AreaNormalCenterData &b)
{
  AreaNormalCenterData joined{};

  joined.area_cos[0] = a.area_cos[0] + b.area_cos[0];
  joined.area_cos[1] = a.area_cos[1] + b.area_cos[1];
  joined.count_co[0] = a.count_co[0] + b.count_co[0];
  joined.count_co[1] = a.count_co[1] + b.count_co[1];

  joined.area_nos[0] = a.area_nos[0] + b.area_nos[0];
  joined.area_nos[1] = a.area_nos[1] + b.area_nos[1];
  joined.count_no[0] = a.count_no[0] + b.count_no[0];
  joined.count_no[1] = a.count_no[1] + b.count_no[1];

  return joined;
}

void calc_area_center(const Depsgraph &depsgraph,
                      const Brush &brush,
                      const Object &ob,
                      const IndexMask &node_mask,
                      float r_area_co[3])
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const SculptSession &ss = *ob.runtime->sculpt_session;
  int n;

  AreaNormalCenterData anctd;
  threading::EnumerableThreadSpecific<SampleLocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_mesh(ob,
                                                    vert_positions,
                                                    vert_normals,
                                                    hide_vert,
                                                    brush,
                                                    AverageDataFlags::Position,
                                                    nodes[i],
                                                    tls,
                                                    anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const bool has_bm_orco = ss.bm && dyntopo::stroke_is_dyntopo(ob, brush);

      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_bmesh(
                  ob, brush, AverageDataFlags::Position, has_bm_orco, nodes[i], tls, anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_grids(
                  ob, brush, AverageDataFlags::Position, nodes[i], tls, anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
  }

  /* For flatten center. */
  for (n = 0; n < anctd.area_cos.size(); n++) {
    if (anctd.count_co[n] == 0) {
      continue;
    }

    mul_v3_v3fl(r_area_co, anctd.area_cos[n], 1.0f / anctd.count_co[n]);
    break;
  }

  if (n == 2) {
    zero_v3(r_area_co);
  }

  if (anctd.count_co[0] == 0 && anctd.count_co[1] == 0) {
    if (ss.cache) {
      copy_v3_v3(r_area_co, ss.cache->location_symm);
    }
  }
}

std::optional<float3> calc_area_normal(const Depsgraph &depsgraph,
                                       const Brush &brush,
                                       const Object &ob,
                                       const IndexMask &node_mask)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  AreaNormalCenterData anctd;
  threading::EnumerableThreadSpecific<SampleLocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_mesh(ob,
                                                    vert_positions,
                                                    vert_normals,
                                                    hide_vert,
                                                    brush,
                                                    AverageDataFlags::Normal,
                                                    nodes[i],
                                                    tls,
                                                    anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const bool has_bm_orco = ss.bm && dyntopo::stroke_is_dyntopo(ob, brush);

      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_bmesh(
                  ob,
                  brush,
                  AverageDataFlags::Normal,
                  has_bm_orco,
                  static_cast<const bke::pbvh::BMeshNode &>(nodes[i]),
                  tls,
                  anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_grids(
                  ob, brush, AverageDataFlags::Normal, nodes[i], tls, anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
  }

  for (const int i : {0, 1}) {
    if (anctd.count_no[i] != 0) {
      if (!math::is_zero(anctd.area_nos[i])) {
        return math::normalize(anctd.area_nos[i]);
      }
    }
  }
  return std::nullopt;
}

/*
 * Stabilizes the position (center) and orientation (normal) of the brush plane during a stroke.
 * Implements a smoothing mechanism based on a weighted moving average for both the plane normal
 * and the plane center.
 *
 * The stabilized normal (`r_stabilized_normal`) is computed as the average of the last
 * `max_normal_index` plane normals, where `max_normal_index` is determined by the
 * `stabilize_normal` parameter of the brush. Each new plane normal is interpolated with the
 * previous plane normal, with `stabilize_normal` controlling the interpolation factor.
 *
 * The stabilized center (`r_stabilized_center`) is computed based on the signed distances
 * of the stored plane centers from a reference plane defined by the current stroke step's center
 * and the stabilized normal. The signed distances are averaged, and this average is used to
 * adjust the position of the stabilized center such that it maintains the average offset of the
 * stored centers relative to the reference plane.
 */
static void calc_stabilized_plane(const Brush &brush,
                                  StrokeCache &cache,
                                  const float3 &plane_normal,
                                  const float3 &plane_center,
                                  float3 &r_stabilized_normal,
                                  float3 &r_stabilized_center)
{
  auto &plane_cache = cache.plane_brush;

  const float normal_weight = brush.stabilize_normal;
  const float center_weight = brush.stabilize_plane;

  float3 new_plane_normal;
  float3 new_plane_center;

  if (plane_cache.first_time) {
    new_plane_normal = plane_normal;
    new_plane_center = plane_center;

    const int max_normal_index = int(1 +
                                     normal_weight * (plane_brush_max_rolling_average_num - 1));
    const int max_center_index = int(1 +
                                     center_weight * (plane_brush_max_rolling_average_num - 1));

    plane_cache.normals.reinitialize(max_normal_index);
    plane_cache.centers.reinitialize(max_center_index);
    plane_cache.normals.fill(plane_normal);
    plane_cache.centers.fill(plane_center);

    plane_cache.normal_index = 0;
    plane_cache.center_index = 0;
    plane_cache.first_time = false;
  }
  else {
    const float3 last_normal = plane_cache.last_normal.value();
    const float3 last_center = plane_cache.last_center.value();

    /* Interpolate between `plane_normal` and the last plane normal. */
    new_plane_normal = math::normalize(
        math::interpolate(plane_normal, last_normal, normal_weight));

    float4 last_plane;
    plane_from_point_normal_v3(last_plane, last_center, last_normal);

    /* Projection of `plane_center` on the last plane. */
    float3 projected_plane_center;
    closest_to_plane_normalized_v3(projected_plane_center, last_plane, plane_center);

    new_plane_center = math::interpolate(plane_center, projected_plane_center, center_weight);
  }

  plane_cache.normals[plane_cache.normal_index] = new_plane_normal;
  plane_cache.centers[plane_cache.center_index] = new_plane_center;

  plane_cache.normal_index = (plane_cache.normal_index + 1) % plane_cache.normals.size();
  plane_cache.center_index = (plane_cache.center_index + 1) % plane_cache.centers.size();

  r_stabilized_normal = float3(0.0f);

  for (const int i : plane_cache.normals.index_range()) {
    r_stabilized_normal += plane_cache.normals[i];
  }
  r_stabilized_normal = math::normalize(r_stabilized_normal);

  float4 reference_plane;
  plane_from_point_normal_v3(reference_plane, new_plane_center, r_stabilized_normal);
  float total_signed_distance = 0.0f;

  for (const int i : plane_cache.centers.index_range()) {
    float signed_distance = math::dot(r_stabilized_normal, plane_cache.centers[i]) -
                            reference_plane.w;
    total_signed_distance += signed_distance;
  }

  const float avg_signed_distance = total_signed_distance / plane_cache.centers.size();
  const float new_center_signed_distance = math::dot(r_stabilized_normal, new_plane_center) -
                                           reference_plane.w;
  const float adjusted_distance = new_center_signed_distance - avg_signed_distance;
  r_stabilized_center = new_plane_center - r_stabilized_normal * adjusted_distance;

  plane_cache.last_normal = r_stabilized_normal;
  plane_cache.last_center = r_stabilized_center;
}

void calc_area_normal_and_center(const Depsgraph &depsgraph,
                                 const Brush &brush,
                                 const Object &ob,
                                 const IndexMask &node_mask,
                                 float r_area_no[3],
                                 float r_area_co[3])
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  int n;

  AreaNormalCenterData anctd;
  threading::EnumerableThreadSpecific<SampleLocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_mesh(ob,
                                                    vert_positions,
                                                    vert_normals,
                                                    hide_vert,
                                                    brush,
                                                    AverageDataFlags::All,
                                                    nodes[i],
                                                    tls,
                                                    anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const bool has_bm_orco = ss.bm && dyntopo::stroke_is_dyntopo(ob, brush);

      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_bmesh(
                  ob, brush, AverageDataFlags::All, has_bm_orco, nodes[i], tls, anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      anctd = threading::parallel_reduce(
          node_mask.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            SampleLocalData &tls = all_tls.local();
            node_mask.slice(range).foreach_index([&](const int i) {
              calc_area_normal_and_center_node_grids(
                  ob, brush, AverageDataFlags::All, nodes[i], tls, anctd);
            });
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
  }

  /* For flatten center. */
  for (n = 0; n < anctd.area_cos.size(); n++) {
    if (anctd.count_co[n] == 0) {
      continue;
    }

    mul_v3_v3fl(r_area_co, anctd.area_cos[n], 1.0f / anctd.count_co[n]);
    break;
  }

  if (n == 2) {
    zero_v3(r_area_co);
  }

  if (anctd.count_co[0] == 0 && anctd.count_co[1] == 0) {
    if (ss.cache) {
      copy_v3_v3(r_area_co, ss.cache->location_symm);
    }
  }

  /* For area normal. */
  for (n = 0; n < anctd.area_nos.size(); n++) {
    if (normalize_v3_v3(r_area_no, anctd.area_nos[n]) != 0.0f) {
      break;
    }
  }

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_PLANE) {
    float3 stabilized_normal;
    float3 stabilized_center;

    calc_stabilized_plane(
        brush, *ss.cache, r_area_no, r_area_co, stabilized_normal, stabilized_center);

    copy_v3_v3(r_area_no, stabilized_normal);
    copy_v3_v3(r_area_co, stabilized_center);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic Brush Utilities
 * \{ */

/**
 * Calculates the sign of the direction of the brush stroke, typically indicates whether the stroke
 * will deform a surface inwards or outwards along the brush normal.
 */
static float brush_flip(const Brush &brush, const StrokeCache &cache)
{
  const float dir = (brush.flag & BRUSH_DIR_IN) ? -1.0f : 1.0f;
  const float invert = cache.toggle_settings.invert ? -1.0f : 1.0f;

  return dir * invert;
}

/**
 * Return modified brush strength. Includes the direction of the brush, positive
 * values pull vertices, negative values push. Uses tablet pressure and a
 * special multiplier found experimentally to scale the strength factor.
 */
static float brush_strength(const Sculpt &sd,
                            const StrokeCache &cache,
                            const float feather,
                            const PaintModeSettings & /*paint_mode_settings*/)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const bke::PaintRuntime &paint_runtime = *sd.paint.runtime;

  /* Primary strength input; square it to make lower values more sensitive. */
  const float root_alpha = BKE_brush_alpha_get(&sd.paint, &brush);
  const float alpha = root_alpha * root_alpha;
  const float pressure = BKE_brush_use_alpha_pressure(&brush) ?
                             BKE_curvemapping_evaluateF(brush.curve_strength, 0, cache.pressure) :
                             1.0f;
  /* SCULPT_ADAPTIVE_STROKE_V1: distance weight is applied once, after
   * brush-specific strength. Avoid combining it with legacy overlap attenuation. */
  float overlap = sculpt_uniform_accumulation_enabled(brush) ?
                      1.0f : paint_runtime.overlap_factor;
  /* Spacing is integer percentage of radius, divide by 50 to get
   * normalized diameter. */

  const float flip = brush_flip(brush, cache);

  /* Pressure final value after being tweaked depending on the brush. */
  float final_pressure;

  switch (brush.sculpt_brush_type) {
    case SCULPT_BRUSH_TYPE_CLAY:
      final_pressure = pow4f(pressure);
      overlap = (1.0f + overlap) / 2.0f;
      return 0.25f * alpha * flip * final_pressure * overlap * feather;
    case SCULPT_BRUSH_TYPE_DRAW:
    case SCULPT_BRUSH_TYPE_DRAW_SHARP:
    case SCULPT_BRUSH_TYPE_LAYER:
      return alpha * flip * pressure * overlap * feather;
    case SCULPT_BRUSH_TYPE_DISPLACEMENT_ERASER:
      return alpha * pressure * overlap * feather;
    case SCULPT_BRUSH_TYPE_CLOTH:
      if (brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_GRAB) {
        /* Grab deform uses the same falloff as a regular grab brush. */
        return root_alpha * feather;
      }
      else if (brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_SNAKE_HOOK) {
        return root_alpha * feather * pressure * overlap;
      }
      else if (brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_EXPAND) {
        /* Expand is more sensible to strength as it keeps expanding the cloth when sculpting over
         * the same vertices. */
        return 0.1f * alpha * flip * pressure * overlap * feather;
      }
      else {
        /* Multiply by 10 by default to get a larger range of strength depending on the size of the
         * brush and object. */
        return 10.0f * alpha * flip * pressure * overlap * feather;
      }
    case SCULPT_BRUSH_TYPE_DRAW_FACE_SETS:
      return alpha * pressure * overlap * feather;
    case SCULPT_BRUSH_TYPE_SLIDE_RELAX:
      return alpha * pressure * overlap * feather * 2.0f;
    case SCULPT_BRUSH_TYPE_PAINT:
      final_pressure = pressure * pressure;
      return final_pressure * overlap * feather;
    case SCULPT_BRUSH_TYPE_SMEAR:
    case SCULPT_BRUSH_TYPE_BLUR:
    case SCULPT_BRUSH_TYPE_DISPLACEMENT_SMEAR:
      return alpha * pressure * overlap * feather;
    case SCULPT_BRUSH_TYPE_CLAY_STRIPS:
      /* Clay Strips needs less strength to compensate the curve. */
      final_pressure = powf(pressure, 1.5f);
      return alpha * flip * final_pressure * overlap * feather * 0.3f;
    case SCULPT_BRUSH_TYPE_CLAY_THUMB:
      final_pressure = pressure * pressure;
      return alpha * flip * final_pressure * overlap * feather * 1.3f;

    case SCULPT_BRUSH_TYPE_MASK:
      overlap = (1.0f + overlap) / 2.0f;
      switch (BrushMaskTool(brush.mask_tool)) {
        case BRUSH_MASK_DRAW:
          return alpha * flip * pressure * overlap * feather;
        case BRUSH_MASK_SMOOTH:
          return alpha * pressure * feather;
      }
      break;
    case SCULPT_BRUSH_TYPE_CREASE:
    case SCULPT_BRUSH_TYPE_BLOB:
      return alpha * flip * pressure * overlap * feather;

    case SCULPT_BRUSH_TYPE_INFLATE:
      if (flip > 0.0f) {
        return 0.250f * alpha * flip * pressure * overlap * feather;
      }
      else {
        return 0.125f * alpha * flip * pressure * overlap * feather;
      }

    case SCULPT_BRUSH_TYPE_MULTIPLANE_SCRAPE:
      overlap = (1.0f + overlap) / 2.0f;
      return alpha * flip * pressure * overlap * feather;

    case SCULPT_BRUSH_TYPE_PLANE:
      if (flip > 0.0f || brush.plane_inversion_mode == BRUSH_PLANE_SWAP_HEIGHT_AND_DEPTH) {
        overlap = (1.0f + overlap) / 2.0f;
        return alpha * pressure * overlap * feather;
      }
      /* When the brush is inverted with the Invert Displacement mode (i.e. when the brush adds
       * contrast), use a different formula that results in a lower strength. This is done because,
       * from an artistic point of view, the contrast would otherwise generally be too strong. Note
       * that this behavior is coherent with the way Fill, Scrape and Flatten work. See #136211. */
      else {
        return 0.5f * alpha * pressure * overlap * feather;
      }
    case SCULPT_BRUSH_TYPE_SMOOTH:
      return flip * alpha * pressure * feather;

    case SCULPT_BRUSH_TYPE_PINCH:
      if (flip > 0.0f) {
        return alpha * flip * pressure * overlap * feather;
      }
      else {
        return 0.25f * alpha * flip * pressure * overlap * feather;
      }

    case SCULPT_BRUSH_TYPE_NUDGE:
      overlap = (1.0f + overlap) / 2.0f;
      return alpha * pressure * overlap * feather;

    case SCULPT_BRUSH_TYPE_THUMB:
      return alpha * pressure * feather;

    case SCULPT_BRUSH_TYPE_SNAKE_HOOK:
      return root_alpha * feather;

    case SCULPT_BRUSH_TYPE_GRAB:
      return root_alpha * feather;

    case SCULPT_BRUSH_TYPE_ROTATE:
      return alpha * pressure * feather;

    case SCULPT_BRUSH_TYPE_ELASTIC_DEFORM:
    case SCULPT_BRUSH_TYPE_POSE:
    case SCULPT_BRUSH_TYPE_BOUNDARY:
      return root_alpha * feather;
    case SCULPT_BRUSH_TYPE_SIMPLIFY:
      /* The Dyntopo Density brush does not use a normal brush workflow to calculate the effect,
       * and this strength value is unused. */
      return 0.0f;
    case SCULPT_BRUSH_TYPE_SCENE_PROJECT:
      return flip * alpha * pressure * overlap * feather;
  }
  BLI_assert_unreachable();
  return 0.0f;
}

void sculpt_apply_texture(const SculptSession &ss,
                          const Brush &brush,
                          const float brush_point[3],
                          const int thread_id,
                          float *r_value,
                          float4 &r_rgba,
                          const float3 *surface_normal,
                          const int ccg_index)
{
  const StrokeCache &cache = *ss.cache;
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);

  if (!mtex->tex) {
    *r_value = 1.0f;
    copy_v4_fl(r_rgba, 1.0f);
    return;
  }

  float point[3];
  sub_v3_v3v3(point, brush_point, cache.plane_offset);

  if (mtex->brush_map_mode == MTEX_MAP_MODE_3D) {
    /* Get strength by feeding the vertex location directly into a texture. */
    *r_value = BKE_brush_sample_tex_3d(cache.paint, &brush, mtex, point, r_rgba, 0, ss.tex_pool);
  }
  else {
    /* If the active area is being applied for symmetry, flip it
     * across the symmetry axis and rotate it back to the original
     * position in order to project it. This insures that the
     * brush texture will be oriented correctly. */
    if (cache.radial_symmetry_pass) {
      mul_m4_v3(cache.symm_rot_mat_inv.ptr(), point);
    }
    float3 symm_point = symmetry_flip(point, cache.mirror_symmetry_pass);

    /* Still no symmetry supported for other paint modes.
     * Sculpt does it DIY. */
    if (mtex->brush_map_mode == MTEX_MAP_MODE_AREA) {
      /* Similar to fixed mode, but projects from brush angle
       * rather than view direction. */

      mul_m4_v3(cache.brush_local_mat.ptr(), symm_point);

      auto *anchor_diag = cache.anchor_projection_diagnostics.get();
      int anchor_mode = 0;
      if (anchor_diag && ccg_index < 0) {
        anchor_diag->missing_ccg.fetch_add(1, std::memory_order_relaxed);
      }
      float x = symm_point[0];
      float y = symm_point[1];

      /* ANCHORED_GEODESIC_V3: cache stores shortest graph paths along the CCG
       * surface. Radius follows surface distance, texture angle stays in the
       * brush-local tangent frame. Samples on unreachable topology are masked
       * rather than painting through nearby disconnected/folded surfaces. */
      const AnchorSurfacePathCache *surface_path = cache.anchor_surface_path.get();
      const bool use_surface_path =
          brush.stroke_method == BRUSH_STROKE_ANCHORED &&
          (brush.flag2 & BRUSH_SCULPT_ANCHOR_SURFACE_PROJECT) &&
          (brush.flag2 & BRUSH_SCULPT_ANCHOR_GEODESIC) && surface_path &&
          surface_path->ready && ccg_index >= 0;
      if (use_surface_path) {
        anchor_mode = 3;
        AnchorV5Sample projection;
        bool reachable;
        if (surface_path->projection_v5) {
          reachable = anchor_v5_sample(*surface_path, ccg_index, projection);
        }
        else {
          const auto found = surface_path->distances.find(ccg_index);
          reachable = found != surface_path->distances.end();
          if (reachable) { projection.distance = found->second; }
          const auto frame = surface_path->frames.find(ccg_index);
          projection.has_uv = frame != surface_path->frames.end();
          if (projection.has_uv) { projection.uv = frame->second.uv; }
        }
        if (!reachable || projection.distance > cache.radius) {
          if (anchor_diag) {
            anchor_diag->samples[5].fetch_add(1, std::memory_order_relaxed);
          }
          *r_value = 0.0f;
          r_rgba = float4(0.0f);
          return;
        }
        bool transported = false;
        if ((brush.flag2 & BRUSH_SCULPT_ANCHOR_TRANSPORT) &&
            surface_path->transport_ready && projection.has_uv && cache.radius > 1.0e-6f)
        {
          x = projection.uv.x / cache.radius;
          y = projection.uv.y / cache.radius;
          transported = true;
          anchor_mode = surface_path->projection_v5 ? 6 : 4;
        }
        if (!transported) {
          const float planar_radius = sqrtf(x * x + y * y);
          if (planar_radius > 0.05f && cache.radius > 1.0e-6f) {
            const float surface_radius = projection.distance / cache.radius;
            const float scale = clamp_f(surface_radius / planar_radius, 1.0f, 2.0f);
            x *= scale;
            y *= scale;
          }
        }
      }

      /* ANCHORED_SURFACE_PROJECTION_V1.
       * Standard uses the unchanged planar Area Plane mapping. With this optional mode,
       * compensate the tangent-plane chord length using an osculating circular arc estimate.
       * This is local curvature compensation, NOT a global geodesic unwrap. It keeps the
       * projected angle, is bounded for folds/high-frequency details, and requires no new
       * topology traversal or persistent Multires buffers. */
      if (!use_surface_path && brush.stroke_method == BRUSH_STROKE_ANCHORED &&
          (brush.flag2 & BRUSH_SCULPT_ANCHOR_SURFACE_PROJECT))
      {
        const float planar_radius = sqrtf(x * x + y * y);
        anchor_mode = 1;
        const float normal_offset = fabsf(symm_point[2]);
        if (planar_radius > 1.0e-5f && normal_offset > 1.0e-6f) {
          /* Circular arc estimate: s / r = (1 + t^2) * atan(t) / t,
           * t = abs(height) / radial tangent-plane distance. */
          const float t = min_ff(normal_offset / planar_radius, 0.65f);
          float arc_scale = min_ff((1.0f + t * t) * atanf(t) / t, 1.30f);

          /* ANCHORED_NORMAL_GUIDED_V2: use the actual SubdivCCG surface orientation
           * to estimate local radial arc length more accurately. This is still a
           * bounded, local approximation and not a geodesic solver. The height-only
           * estimate above stays as a fallback for all other brush paths. */
          if ((brush.flag2 & BRUSH_SCULPT_ANCHOR_NORMAL_GUIDED) && surface_normal) {
            float3 n = *surface_normal;
            if (cache.radial_symmetry_pass) {
              mul_mat3_m4_v3(cache.symm_rot_mat_inv.ptr(), n);
            }
            n = symmetry_flip(n, cache.mirror_symmetry_pass);
            mul_mat3_m4_v3(cache.brush_local_mat.ptr(), n);
            if (normalize_v3(n) > 1.0e-6f && n.z > 0.15f) {
              const float radial_tilt = (n.x * x + n.y * y) / planar_radius;
              /* Avoid projecting across an unrelated facing surface or a hard
               * fold: the surface offset and radial normal must agree. */
              if (-symm_point[2] * radial_tilt > 0.0f) {
                const float theta = min_ff(atan2f(fabsf(radial_tilt), n.z), 1.15f);
                if (theta > 1.0e-4f) {
                  arc_scale = min_ff(theta / sinf(theta), 1.30f);
                  anchor_mode = 2;
                }
              }
            }
          }

          x *= arc_scale;
          y *= arc_scale;
        }
      }

      if (anchor_diag) {
        anchor_diag->samples[anchor_mode].fetch_add(1, std::memory_order_relaxed);
      }
      x *= mtex->size[0];
      y *= mtex->size[1];

      x += mtex->ofs[0];
      y += mtex->ofs[1];

      paint_get_tex_pixel(mtex, x, y, ss.tex_pool, thread_id, r_value, r_rgba);

      add_v3_fl(r_rgba, brush.texture_sample_bias);  // v3 -> Ignore alpha
      *r_value -= brush.texture_sample_bias;
    }
    else {
      const float2 point_2d = ED_view3d_project_float_v2_m4(
          cache.vc->region, symm_point, cache.projection_mat);
      const float point_3d[3] = {point_2d[0], point_2d[1], 0.0f};
      *r_value = BKE_brush_sample_tex_3d(
          cache.paint, &brush, mtex, point_3d, r_rgba, 0, ss.tex_pool);
    }
  }
}

void calc_vertex_displacement(const SculptSession &ss, const Brush &brush, float translation[3])
{
  mul_v3_fl(translation, ss.cache->bstrength);
  /* Handle brush inversion */
  if (ss.cache->bstrength < 0) {
    translation[0] *= -1;
    translation[1] *= -1;
  }

  /* Apply texture size */
  for (int i = 0; i < 3; ++i) {
    translation[i] *= math::safe_divide(1.0f, pow2f(brush.mtex.size[i]));
  }

  /* Transform vector to object space */
  mul_mat3_m4_v3(ss.cache->brush_local_mat_inv.ptr(), translation);

  /* Handle symmetry */
  if (ss.cache->radial_symmetry_pass) {
    mul_m4_v3(ss.cache->symm_rot_mat.ptr(), translation);
  }
  copy_v3_v3(translation, symmetry_flip(translation, ss.cache->mirror_symmetry_pass));
}

bool node_fully_masked_or_hidden(const bke::pbvh::Node &node)
{
  if (BKE_pbvh_node_fully_hidden_get(node)) {
    return true;
  }
  if (BKE_pbvh_node_fully_masked_get(node)) {
    return true;
  }
  return false;
}

bool node_in_sphere(const bke::pbvh::Node &node,
                    const float3 &location,
                    const float radius_sq,
                    const bool original)
{
  const Bounds<float3> &bounds = original ? node.bounds_orig() : node.bounds();
  const float3 nearest = math::clamp(location, bounds.min, bounds.max);
  return math::distance_squared(location, nearest) < radius_sq;
}

bool node_in_cylinder(const DistRayAABB_Precalc &ray_dist_precalc,
                      const bke::pbvh::Node &node,
                      const float radius_sq,
                      const bool original)
{
  const Bounds<float3> &bounds = original ? node.bounds_orig() : node.bounds();

  float dummy_co[3], dummy_depth;
  const float dist_sq = dist_squared_ray_to_aabb_v3(
      &ray_dist_precalc, bounds.min, bounds.max, dummy_co, &dummy_depth);

  /* TODO: Solve issues and enable distance check. */
  return dist_sq < radius_sq || true;
}

static IndexMask pbvh_gather_cursor_update(Object &ob, bool use_original, IndexMaskMemory &memory)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const float3 center = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  return bke::pbvh::search_nodes(pbvh, memory, [&](const bke::pbvh::Node &node) {
    return node_in_sphere(node, center, ss.cursor_radius, use_original);
  });
}

/** \return All nodes that are potentially within the cursor or brush's area of influence. */
static IndexMask pbvh_gather_generic(Object &ob,
                                     const Brush &brush,
                                     const bool use_original,
                                     const float radius_scale,
                                     IndexMaskMemory &memory)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  const float3 center = ss.cache->location_symm;
  const float radius_sq = math::square(ss.cache->radius * radius_scale);
  const bool ignore_ineffective = brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_MASK;
  switch (brush.falloff_shape) {
    case PAINT_FALLOFF_SHAPE_SPHERE: {
      return bke::pbvh::search_nodes(pbvh, memory, [&](const bke::pbvh::Node &node) {
        if (ignore_ineffective && node_fully_masked_or_hidden(node)) {
          return false;
        }
        return node_in_sphere(node, center, radius_sq, use_original);
      });
    }

    case PAINT_FALLOFF_SHAPE_TUBE: {
      const DistRayAABB_Precalc ray_dist_precalc = dist_squared_ray_to_aabb_v3_precalc(
          center, ss.cache->view_normal_symm);
      return bke::pbvh::search_nodes(pbvh, memory, [&](const bke::pbvh::Node &node) {
        if (ignore_ineffective && node_fully_masked_or_hidden(node)) {
          return false;
        }
        return node_in_cylinder(ray_dist_precalc, node, radius_sq, use_original);
      });
    }
  }

  return {};
}

IndexMask gather_nodes(const bke::pbvh::Tree &pbvh,
                       const eBrushFalloffShape falloff_shape,
                       const bool use_original,
                       const float3 &location,
                       const float radius_sq,
                       const std::optional<float3> &ray_direction,
                       IndexMaskMemory &memory)
{
  PRF_scope(ProfileCategory::Editor);
  switch (falloff_shape) {
    case PAINT_FALLOFF_SHAPE_SPHERE: {
      return bke::pbvh::search_nodes(pbvh, memory, [&](const bke::pbvh::Node &node) {
        if (node_fully_masked_or_hidden(node)) {
          return false;
        }
        return node_in_sphere(node, location, radius_sq, use_original);
      });
    }

    case PAINT_FALLOFF_SHAPE_TUBE: {
      BLI_assert(ray_direction);
      const DistRayAABB_Precalc ray_dist_precalc = dist_squared_ray_to_aabb_v3_precalc(
          location, ray_direction.value_or(float3(0.0f)));
      return bke::pbvh::search_nodes(pbvh, memory, [&](const bke::pbvh::Node &node) {
        if (node_fully_masked_or_hidden(node)) {
          return false;
        }
        return node_in_cylinder(ray_dist_precalc, node, radius_sq, use_original);
      });
    }
  }
  BLI_assert_unreachable();
  return {};
}

static IndexMask pbvh_gather_texpaint(Object &ob,
                                      const Brush &brush,
                                      const bool use_original,
                                      const float radius_scale,
                                      IndexMaskMemory &memory)
{
  return pbvh_gather_generic(ob, brush, use_original, radius_scale, memory);
}

/* Calculate primary direction of movement for many brushes. */
static float3 calc_sculpt_normal(const Depsgraph &depsgraph,
                                 const Sculpt &sd,
                                 Object &ob,
                                 const IndexMask &node_mask)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const SculptSession &ss = *ob.runtime->sculpt_session;
  switch (brush.sculpt_plane) {
    case SCULPT_DISP_DIR_AREA:
      return calc_area_normal(depsgraph, brush, ob, node_mask).value_or(float3(0));
    case SCULPT_DISP_DIR_VIEW:
      return ss.cache->view_normal;
    case SCULPT_DISP_DIR_X:
      return float3(1, 0, 0);
    case SCULPT_DISP_DIR_Y:
      return float3(0, 1, 0);
    case SCULPT_DISP_DIR_Z:
      return float3(0, 0, 1);
  }
  BLI_assert_unreachable();
  return {};
}

static void update_sculpt_normal(const Depsgraph &depsgraph,
                                 const Sculpt &sd,
                                 Object &ob,
                                 const brushes::CursorSampleResult &cursor_sample_result)
{
  PRF_scope(ProfileCategory::Editor);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  StrokeCache &cache = *ob.runtime->sculpt_session->cache;
  /* Grab brush does not update the sculpt normal during a stroke. */
  const bool update_normal = !(brush.flag & BRUSH_ORIGINAL_NORMAL) &&
                             !(brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_GRAB) &&
                             !(brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_THUMB &&
                               !(brush.stroke_method == BRUSH_STROKE_ANCHORED)) &&
                             !(brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_ELASTIC_DEFORM) &&
                             !(brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SNAKE_HOOK &&
                               bke::brush::normal_weight_get(brush, cache.toggle_settings.invert) >
                                   0.0f);

  if (cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0 &&
      (stroke_is_first_brush_step_of_symmetry_pass(cache) || update_normal))
  {
    if (cursor_sample_result.plane_normal) {
      cache.sculpt_normal = *cursor_sample_result.plane_normal;
    }
    else {
      cache.sculpt_normal = calc_sculpt_normal(depsgraph, sd, ob, cursor_sample_result.node_mask);
      if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
        project_plane_v3_v3v3(cache.sculpt_normal, cache.sculpt_normal, cache.view_normal_symm);
        normalize_v3(cache.sculpt_normal);
      }
    }
    copy_v3_v3(cache.sculpt_normal_symm, cache.sculpt_normal);
  }
  else {
    cache.sculpt_normal_symm = symmetry_flip(cache.sculpt_normal, cache.mirror_symmetry_pass);
    mul_m4_v3(cache.symm_rot_mat.ptr(), cache.sculpt_normal_symm);
  }
}

static void calc_local_from_screen(const ViewContext &vc,
                                   const float center[3],
                                   const float screen_dir[2],
                                   float r_local_dir[3])
{
  Object &ob = *vc.obact;
  float loc[3];

  mul_v3_m4v3(loc, ob.object_to_world().ptr(), center);
  const float zfac = ED_view3d_calc_zfac(vc.rv3d, loc);

  ED_view3d_win_to_delta(vc.region, screen_dir, zfac, r_local_dir);
  normalize_v3(r_local_dir);

  add_v3_v3(r_local_dir, ob.loc);
  mul_m4_v3(ob.world_to_object().ptr(), r_local_dir);
}

static void calc_brush_local_mat(const float rotation,
                                 const Object &ob,
                                 float local_mat[4][4],
                                 float local_mat_inv[4][4])
{
  const StrokeCache *cache = ob.runtime->sculpt_session->cache;
  float tmat[4][4];
  float mat[4][4];
  float scale[4][4];
  float angle, v[3];

  /* Ensure `ob.world_to_object` is up to date. */
  invert_m4_m4(ob.runtime->world_to_object.ptr(), ob.object_to_world().ptr());

  /* Initialize last column of matrix. */
  mat[0][3] = 0.0f;
  mat[1][3] = 0.0f;
  mat[2][3] = 0.0f;
  mat[3][3] = 1.0f;

  /* Read rotation (user angle, rake, etc.) to find the view's movement direction (negative X of
   * the brush). */
  angle = rotation + cache->special_rotation;
  /* By convention, motion direction points down the brush's Y axis, the angle represents the X
   * axis, normal is a 90 deg CCW rotation of the motion direction. */
  float motion_normal_screen[2];
  motion_normal_screen[0] = cosf(angle);
  motion_normal_screen[1] = sinf(angle);
  /* Convert view's brush transverse direction to object-space,
   * i.e. the normal of the plane described by the motion */
  float motion_normal_local[3];
  calc_local_from_screen(
      *cache->vc, cache->location_symm, motion_normal_screen, motion_normal_local);

  /* Calculate the movement direction for the local matrix.
   * Note that there is a deliberate prioritization here: Our calculations are
   * designed such that the _motion vector_ gets projected into the tangent space;
   * in most cases this will be more intuitive than projecting the transverse
   * direction (which is orthogonal to the motion direction and therefore less
   * apparent to the user).
   * The Y-axis of the brush-local frame has to lie in the intersection of the tangent plane
   * and the motion plane. */

  cross_v3_v3v3(v, cache->sculpt_normal, motion_normal_local);
  normalize_v3_v3(mat[1], v);

  /* Get other axes. */
  cross_v3_v3v3(mat[0], mat[1], cache->sculpt_normal);
  copy_v3_v3(mat[2], cache->sculpt_normal);

  /* Set location. */
  copy_v3_v3(mat[3], cache->location_symm);

  /* Scale by brush radius. */
  float radius = cache->radius;

  normalize_m4(mat);
  scale_m4_fl(scale, radius);
  mul_m4_m4m4(tmat, mat, scale);

  /* Return tmat as is (for converting from local area coords to model-space coords). */
  copy_m4_m4(local_mat_inv, tmat);
  /* Return inverse (for converting from model-space coords to local area coords). */
  invert_m4_m4(local_mat, tmat);
}

float3 tilt_apply_to_normal(const Object &object,
                            const float4x4 &view_inverse,
                            const float3 &normal,
                            const float2 &tilt,
                            const float tilt_strength)
{
  const float3 world_space = math::transform_direction(object.object_to_world(), normal);

  /* Tweaked based on initial user feedback, with a value of 1.0, higher brush tilt strength
   * lead to the stroke surface direction becoming inverted due to extreme rotations. */
  constexpr float tilt_sensitivity = 0.7f;
  const float rot_max = M_PI_2 * tilt_strength * tilt_sensitivity;
  const float3 normal_tilt_y = math::rotate_direction_around_axis(
      world_space, view_inverse.x_axis(), tilt.y * rot_max);
  const float3 normal_tilt_xy = math::rotate_direction_around_axis(
      normal_tilt_y, view_inverse.y_axis(), tilt.x * rot_max);

  return math::normalize(math::transform_direction(object.world_to_object(), normal_tilt_xy));
}

float3 tilt_apply_to_normal(const float3 &normal,
                            const StrokeCache &cache,
                            const float tilt_strength)
{
  return tilt_apply_to_normal(
      *cache.vc->obact, float4x4(cache.vc->rv3d->viewinv), normal, cache.tilt, tilt_strength);
}

float3 tilt_effective_normal_get(const SculptSession &ss, const Brush &brush)
{
  return tilt_apply_to_normal(ss.cache->sculpt_normal_symm, *ss.cache, brush.tilt_strength_factor);
}

static void update_brush_local_mat(const Sculpt &sd, Object &ob)
{
  PRF_scope(ProfileCategory::Editor);
  StrokeCache *cache = ob.runtime->sculpt_session->cache;

  if (cache->mirror_symmetry_pass == 0 && cache->radial_symmetry_pass == 0) {
    const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
    const MTex *mask_tex = BKE_brush_mask_texture_get(brush, OB_MODE_SCULPT);
    calc_brush_local_mat(
        mask_tex->rot, ob, cache->brush_local_mat.ptr(), cache->brush_local_mat_inv.ptr());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture painting
 * \{ */

static bool sculpt_needs_pbvh_pixels(const Brush &brush, const Object &ob)
{
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT &&
      USER_EXPERIMENTAL_TEST(&U, use_sculpt_texture_paint))
  {
    return ob.runtime->sculpt_session->cache->image_data.get();
  }

  return false;
}

static void sculpt_pbvh_update_pixels(const Depsgraph &depsgraph, Object &ob)
{
  BLI_assert(ob.type == OB_MESH);

  StrokeCache &cache = *ob.runtime->sculpt_session->cache;
  if (!cache.image_data) {
    return;
  }

  bke::pbvh::build_pixels(depsgraph, ob, *cache.image_data->image, *cache.image_data->image_user);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic Brush Plane & Symmetry Utilities
 * \{ */
struct RaycastData {
  Object *object;
  float3 ray_start;
  float3 ray_normal;
  bool hit;
  float depth;
  bool is_mid_stroke;
  bool use_original;
  Span<float3> vert_positions;
  OffsetIndices<int> faces;
  Span<int> corner_verts;
  Span<int3> corner_tris;
  VArraySpan<bool> hide_poly;

  const SubdivCCG *subdiv_ccg;

  ActiveVert active_vertex = {};
  float3 face_normal;

  int active_face_grid_index;

  IsectRayPrecalc isect_precalc;
};

struct FindNearestToRayData {
  Object *object;
  float3 ray_start;
  float3 ray_normal;
  bool hit;
  float depth;
  float dist_sq_to_ray;
  bool is_mid_stroke;
  bool use_original;
  Span<float3> vert_positions;
  OffsetIndices<int> faces;
  Span<int> corner_verts;
  Span<int3> corner_tris;
  VArraySpan<bool> hide_poly;

  const SubdivCCG *subdiv_ccg;
};

ePaintSymmetryAreas get_vertex_symm_area(const float co[3])
{
  ePaintSymmetryAreas symm_area = ePaintSymmetryAreas(PAINT_SYMM_AREA_DEFAULT);
  if (co[0] < 0.0f) {
    symm_area |= PAINT_SYMM_AREA_X;
  }
  if (co[1] < 0.0f) {
    symm_area |= PAINT_SYMM_AREA_Y;
  }
  if (co[2] < 0.0f) {
    symm_area |= PAINT_SYMM_AREA_Z;
  }
  return symm_area;
}

static void flip_qt_qt(float out[4], const float in[4], const ePaintSymmetryFlags symm)
{
  float axis[3], angle;

  quat_to_axis_angle(axis, &angle, in);
  normalize_v3(axis);

  if (symm & PAINT_SYMM_X) {
    axis[0] *= -1.0f;
    angle *= -1.0f;
  }
  if (symm & PAINT_SYMM_Y) {
    axis[1] *= -1.0f;
    angle *= -1.0f;
  }
  if (symm & PAINT_SYMM_Z) {
    axis[2] *= -1.0f;
    angle *= -1.0f;
  }

  axis_angle_normalized_to_quat(out, axis, angle);
}

static void flip_qt(float quat[4], const ePaintSymmetryFlags symm)
{
  flip_qt_qt(quat, quat, symm);
}

float3 flip_v3_by_symm_area(const float3 &vector,
                            const ePaintSymmetryFlags symm,
                            const ePaintSymmetryAreas symmarea,
                            const float3 &pivot)
{
  float3 result = vector;
  for (int i = 0; i < 3; i++) {
    ePaintSymmetryFlags symm_it = ePaintSymmetryFlags(1 << i);
    if (!(symm & symm_it)) {
      continue;
    }
    if (symmarea & ePaintSymmetryAreas(symm_it)) {
      result = symmetry_flip(result, symm_it);
    }
    if (pivot[i] < 0.0f) {
      result = symmetry_flip(result, symm_it);
    }
  }
  return result;
}

void flip_quat_by_symm_area(float quat[4],
                            const ePaintSymmetryFlags symm,
                            const ePaintSymmetryAreas symmarea,
                            const float pivot[3])
{
  for (int i = 0; i < 3; i++) {
    ePaintSymmetryFlags symm_it = ePaintSymmetryFlags(1 << i);
    if (!(symm & symm_it)) {
      continue;
    }
    if (symmarea & ePaintSymmetryAreas(symm_it)) {
      flip_qt(quat, symm_it);
    }
    if (pivot[i] < 0.0f) {
      flip_qt(quat, symm_it);
    }
  }
}

void calc_brush_plane(const Depsgraph &depsgraph,
                      const Brush &brush,
                      Object &ob,
                      const IndexMask &node_mask,
                      float3 &r_area_no,
                      float3 &r_area_co)
{
  const SculptSession &ss = *ob.runtime->sculpt_session;

  r_area_no = float3(0.0f);
  r_area_co = float3(0.0f);

  const bool use_original_plane = (brush.flag & BRUSH_ORIGINAL_PLANE) &&
                                  brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_PLANE;
  const bool use_original_normal = (brush.flag & BRUSH_ORIGINAL_NORMAL) &&
                                   brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_PLANE;

  const bool needs_recalculation = stroke_is_first_brush_step_of_symmetry_pass(*ss.cache) ||
                                   !use_original_plane || !use_original_normal;

  if (stroke_is_main_symmetry_pass(*ss.cache) && needs_recalculation) {
    switch (brush.sculpt_plane) {
      case SCULPT_DISP_DIR_VIEW:
        r_area_no = ss.cache->view_normal;
        break;

      case SCULPT_DISP_DIR_X:
        r_area_no = float3(1.0f, 0.0f, 0.0f);
        break;

      case SCULPT_DISP_DIR_Y:
        r_area_no = float3(0.0f, 1.0f, 0.0f);
        break;

      case SCULPT_DISP_DIR_Z:
        r_area_no = float3(0.0f, 0.0f, 1.0f);
        break;

      case SCULPT_DISP_DIR_AREA:
        calc_area_normal_and_center(depsgraph, brush, ob, node_mask, r_area_no, r_area_co);
        if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
          project_plane_v3_v3v3(r_area_no, r_area_no, ss.cache->view_normal_symm);
          r_area_no = math::normalize(r_area_no);
        }
        break;
    }

    /* Flatten center has not been calculated yet if we are not using the area normal. */
    if (brush.sculpt_plane != SCULPT_DISP_DIR_AREA) {
      BLI_assert(math::is_zero(r_area_co));
      calc_area_center(depsgraph, brush, ob, node_mask, r_area_co);
    }

    if (!stroke_is_first_brush_step_of_symmetry_pass(*ss.cache) && use_original_normal) {
      r_area_no = ss.cache->sculpt_normal;
    }
    else {
      ss.cache->sculpt_normal = r_area_no;
    }

    if (!stroke_is_first_brush_step_of_symmetry_pass(*ss.cache) && use_original_plane) {
      r_area_co = ss.cache->last_center;
    }
    else {
      ss.cache->last_center = r_area_co;
    }
  }
  else {
    BLI_assert(math::is_zero(ss.cache->symm_rot_mat.location().xyz()));

    r_area_no = ss.cache->sculpt_normal;
    r_area_no = symmetry_flip(r_area_no, ss.cache->mirror_symmetry_pass);
    r_area_no = math::transform_direction(ss.cache->symm_rot_mat, r_area_no);

    r_area_co = ss.cache->last_center;
    r_area_co = symmetry_flip(r_area_co, ss.cache->mirror_symmetry_pass);
    r_area_co = math::transform_point(ss.cache->symm_rot_mat, r_area_co);

    /* Shift the plane for the current tile. */
    r_area_co += ss.cache->plane_offset;
  }
}

float brush_plane_offset_get(const Brush &brush, const SculptSession &ss)
{
  return brush.flag & BRUSH_OFFSET_PRESSURE ? brush.plane_offset * ss.cache->pressure :
                                              brush.plane_offset;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Brush Utilities
 * \{ */

static void dynamic_topology_update(const Depsgraph &depsgraph,
                                    const Scene & /*scene*/,
                                    Sculpt &sd,
                                    Object &ob,
                                    const Brush &brush,
                                    PaintModeSettings & /*paint_mode_settings*/)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  /* Build a list of all nodes that are potentially within the brush's area of influence. */
  const bool use_original = brush_type_needs_original(brush.sculpt_brush_type) ? true :
                                                                                 !ss.cache->accum;
  constexpr float radius_scale = 1.25f;

  IndexMaskMemory memory;
  const IndexMask node_mask = pbvh_gather_generic(ob, brush, use_original, radius_scale, memory);
  if (node_mask.is_empty()) {
    return;
  }

  MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();

  /* Free index based vertex info as it will become invalid after modifying the topology during the
   * stroke. */
  ss.boundary_info_cache.reset();

  PBVHTopologyUpdateMode mode = PBVHTopologyUpdateMode(0);

  if (!(sd.flags & SCULPT_DYNTOPO_DETAIL_MANUAL)) {
    if (sd.flags & SCULPT_DYNTOPO_SUBDIVIDE) {
      mode |= PBVH_Subdivide;
    }

    if ((sd.flags & SCULPT_DYNTOPO_COLLAPSE) ||
        (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SIMPLIFY))
    {
      mode |= PBVH_Collapse;
    }
  }

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
    undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Mask);
  }
  else {
    undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Position);
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.tag_topology_changed(node_mask);
  node_mask.foreach_index([&](const int i) { BKE_pbvh_node_mark_topology_update(nodes[i]); });
  node_mask.foreach_index(
      [&](const int i) { BKE_pbvh_bmesh_node_save_orig(ss.bm, ss.bm_log, &nodes[i], false); },
      exec_mode::grain_size(1));

  float max_edge_len;
  if (sd.flags & (SCULPT_DYNTOPO_DETAIL_CONSTANT | SCULPT_DYNTOPO_DETAIL_MANUAL)) {
    max_edge_len = dyntopo::detail_size::constant_to_detail_size(sd.constant_detail, ob);
  }
  else if (sd.flags & SCULPT_DYNTOPO_DETAIL_BRUSH) {
    max_edge_len = dyntopo::detail_size::brush_to_detail_size(sd.detail_percent, ss.cache->radius);
  }
  else {
    max_edge_len = dyntopo::detail_size::relative_to_detail_size(
        sd.detail_size, ss.cache->radius, ss.cache->dyntopo_pixel_radius, U.pixelsize);
  }
  const float min_edge_len = max_edge_len * dyntopo::detail_size::EDGE_LENGTH_MIN_FACTOR;

  bke::pbvh::bmesh_update_topology(*ss.bm,
                                   pbvh,
                                   *ss.bm_log,
                                   mode,
                                   min_edge_len,
                                   max_edge_len,
                                   ss.cache->location_symm,
                                   ss.cache->view_normal_symm,
                                   ss.cache->radius,
                                   (brush.flag & BRUSH_FRONTFACE) != 0,
                                   (brush.falloff_shape != PAINT_FALLOFF_SHAPE_SPHERE));
}

static bool brush_type_needs_all_pbvh_nodes(const Brush &brush)
{
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_ELASTIC_DEFORM) {
    /* Elastic deformations in any brush need all nodes to avoid artifacts as the effect
     * of the Kelvinlet is not constrained by the radius. */
    return true;
  }

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_POSE) {
    /* Pose needs all nodes because it applies all symmetry iterations at the same time
     * and the IK chain can grow to any area of the model. */
    /* TODO: This can be optimized by filtering the nodes after calculating the chain. */
    return true;
  }

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_BOUNDARY) {
    /* Boundary needs all nodes because it is not possible to know where the boundary
     * deformation is going to be propagated before calculating it. */
    /* TODO: after calculating the boundary info in the first iteration, it should be
     * possible to get the nodes that have vertices included in any boundary deformation
     * and cache them. */
    return true;
  }

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SNAKE_HOOK &&
      brush.snake_hook_deform_type == BRUSH_SNAKE_HOOK_DEFORM_ELASTIC)
  {
    /* Snake hook in elastic deform type has same requirements as the elastic deform brush. */
    return true;
  }
  return false;
}

/** Calculates the nodes that a brush will influence. */
static brushes::CursorSampleResult calc_brush_node_mask(const Depsgraph &depsgraph,
                                                        Object &ob,
                                                        const Brush &brush,
                                                        IndexMaskMemory &memory)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  const bool use_original = brush_type_needs_original(brush.sculpt_brush_type) ? true :
                                                                                 !ss.cache->accum;
  /* Build a list of all nodes that are potentially within the brush's area of influence */

  if (brush_type_needs_all_pbvh_nodes(brush)) {
    /* These brushes need to update all nodes as they are not constrained by the brush radius */
    return {all_leaf_nodes(pbvh, memory), std::nullopt, std::nullopt};
  }
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_PLANE) {
    return brushes::plane::calc_node_mask(depsgraph, ob, brush, memory);
  }
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLAY_STRIPS) {
    return brushes::clay_strips::calc_node_mask(depsgraph, ob, brush, memory);
  }
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH) {
    return {cloth::brush_affected_nodes_gather(ob, brush, memory), std::nullopt, std::nullopt};
  }

  float radius_scale = 1.0f;
  /* Corners of square brushes can go outside the brush radius. */
  if (BKE_brush_has_cube_tip(&brush, PaintMode::Sculpt)) {
    radius_scale = M_SQRT2;
  }

  /* With these options enabled not all required nodes are inside the original brush radius, so
   * the brush can produce artifacts in some situations. */
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW && brush.flag & BRUSH_ORIGINAL_NORMAL) {
    radius_scale = 2.0f;
  }
  return {pbvh_gather_generic(ob, brush, use_original, radius_scale, memory),
          std::nullopt,
          std::nullopt};
}

static void push_undo_nodes(const Depsgraph &depsgraph,
                            Object &ob,
                            const Brush &brush,
                            const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *ob.runtime->sculpt_session;
  bool need_coords = ss.cache->supports_gravity;

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW_FACE_SETS) {
    /* Draw face sets in smooth mode moves the vertices. */
    if (ss.cache->toggle_settings.alt_smooth) {
      need_coords = true;
    }
    else {
      undo::push_nodes(depsgraph, ob, node_mask, undo::Type::FaceSet);
    }
  }
  else if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
    undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Mask);
  }
  else if (brush_type_is_paint(brush.sculpt_brush_type)) {
    undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Color);
  }
  else {
    need_coords = true;
  }

  if (need_coords) {
    undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Position);
  }
}

static const char *sculpt_brush_type_name(const Brush &brush)
{
  switch (eBrushSculptType(brush.sculpt_brush_type)) {
    case SCULPT_BRUSH_TYPE_DRAW:
      return "Draw Brush";
    case SCULPT_BRUSH_TYPE_SMOOTH:
      return "Smooth Brush";
    case SCULPT_BRUSH_TYPE_CREASE:
      return "Crease Brush";
    case SCULPT_BRUSH_TYPE_BLOB:
      return "Blob Brush";
    case SCULPT_BRUSH_TYPE_PINCH:
      return "Pinch Brush";
    case SCULPT_BRUSH_TYPE_INFLATE:
      return "Inflate Brush";
    case SCULPT_BRUSH_TYPE_GRAB:
      return "Grab Brush";
    case SCULPT_BRUSH_TYPE_NUDGE:
      return "Nudge Brush";
    case SCULPT_BRUSH_TYPE_THUMB:
      return "Thumb Brush";
    case SCULPT_BRUSH_TYPE_LAYER:
      return "Layer Brush";
    case SCULPT_BRUSH_TYPE_CLAY:
      return "Clay Brush";
    case SCULPT_BRUSH_TYPE_CLAY_STRIPS:
      return "Clay Strips Brush";
    case SCULPT_BRUSH_TYPE_CLAY_THUMB:
      return "Clay Thumb Brush";
    case SCULPT_BRUSH_TYPE_SNAKE_HOOK:
      return "Snake Hook Brush";
    case SCULPT_BRUSH_TYPE_ROTATE:
      return "Rotate Brush";
    case SCULPT_BRUSH_TYPE_MASK:
      return "Mask Brush";
    case SCULPT_BRUSH_TYPE_SIMPLIFY:
      return "Simplify Brush";
    case SCULPT_BRUSH_TYPE_DRAW_SHARP:
      return "Draw Sharp Brush";
    case SCULPT_BRUSH_TYPE_ELASTIC_DEFORM:
      return "Elastic Deform Brush";
    case SCULPT_BRUSH_TYPE_POSE:
      return "Pose Brush";
    case SCULPT_BRUSH_TYPE_MULTIPLANE_SCRAPE:
      return "Multi-plane Scrape Brush";
    case SCULPT_BRUSH_TYPE_SLIDE_RELAX:
      return "Slide/Relax Brush";
    case SCULPT_BRUSH_TYPE_BOUNDARY:
      return "Boundary Brush";
    case SCULPT_BRUSH_TYPE_CLOTH:
      return "Cloth Brush";
    case SCULPT_BRUSH_TYPE_DRAW_FACE_SETS:
      return "Draw Face Sets";
    case SCULPT_BRUSH_TYPE_DISPLACEMENT_ERASER:
      return "Multires Displacement Eraser";
    case SCULPT_BRUSH_TYPE_DISPLACEMENT_SMEAR:
      return "Multires Displacement Smear";
    case SCULPT_BRUSH_TYPE_PAINT:
      return "Paint Brush";
    case SCULPT_BRUSH_TYPE_SMEAR:
      return "Smear Brush";
    case SCULPT_BRUSH_TYPE_PLANE:
      return "Plane Brush";
    case SCULPT_BRUSH_TYPE_BLUR:
      return "Blur Brush";
    case SCULPT_BRUSH_TYPE_SCENE_PROJECT:
      return "Scene Project Brush";
  }

  return "Sculpting";
}

/* Sculpt Mesh V3 memory pressure probe.
 *
 * Diagnostic only. On Windows this samples process working-set/private bytes,
 * cumulative page faults and currently available physical memory. The API is
 * resolved dynamically so no Psapi import library change is required. */
struct SculptMeshV3MemorySample {
  uint64_t page_faults = 0;
  uint64_t working_set = 0;
  uint64_t private_bytes = 0;
  uint64_t avail_phys = 0;
  bool valid = false;
};

static SculptMeshV3MemorySample sculpt_mesh_v3_memory_sample()
{
  SculptMeshV3MemorySample result;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);

  using GetProcessMemoryInfoFn = BOOL(WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
  static GetProcessMemoryInfoFn get_process_memory_info = []() -> GetProcessMemoryInfoFn {
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
      if (FARPROC fn = GetProcAddress(kernel, "K32GetProcessMemoryInfo")) {
        return reinterpret_cast<GetProcessMemoryInfoFn>(fn);
      }
    }
    if (HMODULE psapi = LoadLibraryW(L"psapi.dll")) {
      if (FARPROC fn = GetProcAddress(psapi, "GetProcessMemoryInfo")) {
        return reinterpret_cast<GetProcessMemoryInfoFn>(fn);
      }
    }
    return nullptr;
  }();

  if (get_process_memory_info &&
      get_process_memory_info(GetCurrentProcess(),
                              reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),
                              sizeof(counters)))
  {
    result.page_faults = counters.PageFaultCount;
    result.working_set = counters.WorkingSetSize;
    result.private_bytes = counters.PrivateUsage;
    result.valid = true;
  }

  MEMORYSTATUSEX memory_status{};
  memory_status.dwLength = sizeof(memory_status);
  if (GlobalMemoryStatusEx(&memory_status)) {
    result.avail_phys = memory_status.ullAvailPhys;
  }
#endif
  return result;
}
/* ANCHORED_SURFACE_TRANSPORT_V4: minimal normal-to-normal rotation of the
 * tangent basis along each accepted shortest-path edge. This is a discrete
 * frame transport, not an exact smooth exponential map. */
static bool anchor_transport_tangent_x(const float3 &parent_axis,
                                      const float3 &parent_normal,
                                      const float3 &next_normal,
                                      float3 &r_axis)
{
  float3 n0 = parent_normal;
  float3 n1 = next_normal;
  if (!anchor_safe_normalize(n0) || !anchor_safe_normalize(n1)) {
    return false;
  }
  const float dot_normals = math::dot(n0, n1);
  if (!std::isfinite(dot_normals) || dot_normals <= -0.25f) {
    /* Ambiguous frame near a normal inversion or a hard fold: use V3. */
    return false;
  }
  const float3 cross_normals = math::cross(n0, n1);
  float3 axis = parent_axis + math::cross(cross_normals, parent_axis) +
                math::cross(cross_normals, math::cross(cross_normals, parent_axis)) /
                    (1.0f + dot_normals);
  axis -= math::dot(axis, n1) * n1;
  if (!anchor_safe_normalize(axis)) {
    return false;
  }
  r_axis = axis;
  return true;
}

/* ANCHORED_SURFACE_PROJECTION_V5: sparse control lattice over the current stamp.
 * CCG seam duplicates share canonical nodes. No global mesh buffers or visited cap. */
static bool anchor_projection_v5_enabled()
{
  static const bool enabled = []() {
    const char *value = std::getenv("BLENDER_ANCHOR_PROJECTION_V5");
    return !(value && value[0] == '0');
  }();
  return enabled;
}

static uint64_t anchor_v51_block_key(const AnchorSurfacePathCache &path,
                                      const int grid, const int cx, const int cy)
{
  return (uint64_t(grid) * path.blocks_per_row + cy / 8) * path.blocks_per_row + cx / 8;
}

static const AnchorV5Sample *anchor_v51_control(const AnchorSurfacePathCache &path,
                                               const int index)
{
  const int grid = index / path.grid_area, local = index % path.grid_area;
  const int cx = (local % path.grid_size) / path.stride;
  const int cy = (local / path.grid_size) / path.stride;
  const auto block = path.control_blocks.find(anchor_v51_block_key(path, grid, cx, cy));
  const int slot = (cy % 8) * 8 + cx % 8;
  if (block == path.control_blocks.end() || !(block->second.present & (uint64_t(1) << slot))) {
    return nullptr;
  }
  return &block->second.values[slot];
}

/* Read-only worker sampling: one block lookup in the common case, with explicit
 * occupancy checks even when adjacent controls lie on a sparse block boundary. */
static bool anchor_v5_sample(const AnchorSurfacePathCache &path,
                             const int ccg_index, AnchorV5Sample &sample)
{
  if (ccg_index < 0 || ccg_index >= path.position_count || path.stride <= 0 ||
      path.grid_size <= 1 || path.grid_area <= 0 || path.blocks_per_row <= 0)
  { return false; }
  const int grid = ccg_index / path.grid_area, local = ccg_index % path.grid_area;
  const int x = local % path.grid_size, y = local / path.grid_size;
  const int cx = x >> path.stride_shift, cy = y >> path.stride_shift;
  const int cx1 = std::min(cx + 1, path.last_control);
  const int cy1 = std::min(cy + 1, path.last_control);
  const float tx = cx1 == cx ? 0.0f : float(x & (path.stride - 1)) * path.inverse_stride;
  const float ty = cy1 == cy ? 0.0f : float(y & (path.stride - 1)) * path.inverse_stride;
  const int xs[4] = {cx, cx1, cx, cx1}, ys[4] = {cy, cy, cy1, cy1};
  const float weights[4] = {(1.0f-tx)*(1.0f-ty), tx*(1.0f-ty), (1.0f-tx)*ty, tx*ty};
  const uint64_t first_key = anchor_v51_block_key(path, grid, cx, cy);
  const auto first = path.control_blocks.find(first_key);
  if (first == path.control_blocks.end()) { return false; }
  sample = AnchorV5Sample();
  const bool same_block = ((cx & 7) != 7 || cx1 == cx) &&
                          ((cy & 7) != 7 || cy1 == cy);
  for (int i = 0; i < 4; ++i) {
    if (weights[i] == 0.0f) { continue; }
    const AnchorV51ControlBlock *block = &first->second;
    if (!same_block && anchor_v51_block_key(path, grid, xs[i], ys[i]) != first_key) {
      const uint64_t key = anchor_v51_block_key(path, grid, xs[i], ys[i]);
      const auto found = path.control_blocks.find(key);
      if (found == path.control_blocks.end()) { return false; }
      block = &found->second;
    }
    const int slot = (ys[i] % 8) * 8 + xs[i] % 8;
    if (!(block->present & (uint64_t(1) << slot))) { return false; }
    const auto &control = block->values[slot];
    sample.distance += weights[i] * control.distance;
    sample.has_uv = sample.has_uv && control.has_uv;
    sample.uv = sample.uv + weights[i] * control.uv;
  }
  if (path.radial_correction) {
    if (sample.has_uv) {
      sample.distance=std::max(0.0f,math::length(sample.uv)+sample.distance);
    }
    else {
      /* A mixed frame still needs the existing V3 distance fallback. Recover
       * full distances from residuals only in this uncommon branch. */
      sample.distance=0.0f;
      for (int i=0; i<4; ++i) {
        if (weights[i]==0.0f) { continue; }
        const int raw=grid*path.grid_area+ys[i]*path.stride*path.grid_size+xs[i]*path.stride;
        const auto *control=anchor_v51_control(path,raw);
        if (!control) { return false; }
        sample.distance+=weights[i]*(control->distance+(control->has_uv?math::length(control->uv):0.0f));
      }
    }
  }
  if (sample.has_uv) {
    const float2 uv = sample.uv;
    sample.uv = {path.uv_rotation.x*uv.x + path.uv_rotation.y*uv.y,
                -path.uv_rotation.y*uv.x + path.uv_rotation.x*uv.y};
  }
  return std::isfinite(sample.distance) && (!sample.has_uv ||
      (std::isfinite(sample.uv.x) && std::isfinite(sample.uv.y)));
}

static bool anchor_v5_set_rotation(AnchorSurfacePathCache &path, float3 current_axis)
{
  current_axis -= math::dot(current_axis, path.reference_normal) * path.reference_normal;
  if (!anchor_safe_normalize(current_axis)) { return false; }
  const float3 reference_y = path.handedness *
                            math::cross(path.reference_normal, path.reference_axis);
  path.uv_rotation = {math::dot(current_axis, path.reference_axis),
                      math::dot(current_axis, reference_y)};
  return true;
}

static int anchor_v5_canonical(const SubdivCCG &ccg,
                               const CCGKey &key,
                               AnchorSurfacePathCache &path,
                               const int index)
{
  const auto found = path.aliases.find(index);
  if (found != path.aliases.end()) { return found->second; }
  const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, index);
  if (coord.x != 0 && coord.y != 0 && coord.x != key.grid_size - 1 &&
      coord.y != key.grid_size - 1) {
    path.aliases.emplace(index, index);
    return index;
  }
  SubdivCCGNeighbors neighbors;
  BKE_subdiv_ccg_neighbor_coords_get(ccg, coord, true, neighbors);
  int canonical = index;
  for (const SubdivCCGCoord &duplicate : neighbors.duplicates()) {
    canonical = std::min(canonical, duplicate.to_index(key));
  }
  path.aliases.emplace(index, canonical);
  for (const SubdivCCGCoord &duplicate : neighbors.duplicates()) {
    path.aliases.insert_or_assign(duplicate.to_index(key), canonical);
  }
  return canonical;
}

struct AnchorV5MetricEdge {
  int a, b;
  float length;
  double weight = 0.0;
};
struct AnchorV5Triangle {
  int a, b, c;
  float sign;
};
static float anchor_v5_area(const float2 &a, const float2 &b, const float2 &c)
{
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/* Small fixed iteration count, not a sample limit. Stress minimization distributes
 * distortion between neighboring controls instead of trusting one predecessor. */
static void anchor_v5_relax(AnchorSurfacePathCache &path,
                            const int fixed_seed,
                            const std::vector<AnchorV5MetricEdge> &graph_edges)
{
  std::vector<int> indices;
  indices.reserve(path.frames.size());
  for (const auto &entry : path.frames) { indices.push_back(entry.first); }
  std::sort(indices.begin(), indices.end());
  std::unordered_map<int, int> slots;
  slots.reserve(indices.size());
  std::vector<float2> initial, uv;
  initial.reserve(indices.size());
  for (const int index : indices) {
    slots.emplace(index, int(initial.size()));
    initial.push_back(path.frames.at(index).uv);
  }
  uv = initial;
  std::vector<AnchorV5MetricEdge> edges;
  for (const auto &edge : graph_edges) {
    const auto a = slots.find(edge.a), b = slots.find(edge.b);
    if (a != slots.end() && b != slots.end() && edge.length > 1.0e-12f) {
      edges.push_back({a->second, b->second, edge.length,
                       1.0 / std::max(double(edge.length) * edge.length, 1.0e-20)});
    }
  }
  std::vector<AnchorV5Triangle> triangles;
  /* Enumerate only grids touched by this patch; duplicated seam vertices use slots
   * of their canonical controls. No array sized to ccg.grids_num is allocated. */
  for (const auto &entry : path.aliases) {
    const int local = entry.first % path.grid_area;
    const int x = local % path.grid_size, y = local / path.grid_size;
    if (x + path.stride >= path.grid_size || y + path.stride >= path.grid_size) {
      continue;
    }
    const int corners[4] = {entry.first, entry.first + path.stride,
        entry.first + path.stride * path.grid_size,
        entry.first + path.stride * (path.grid_size + 1)};
    int slot[4];
    bool valid = true;
    for (int k = 0; k < 4; ++k) {
      const auto alias = path.aliases.find(corners[k]);
      if (alias == path.aliases.end()) { valid = false; break; }
      const auto found = slots.find(alias->second);
      if (found == slots.end()) { valid = false; break; }
      slot[k] = found->second;
    }
    if (!valid) { continue; }
    for (const auto &tri : {std::array<int, 3>{slot[0], slot[1], slot[3]},
                            std::array<int, 3>{slot[0], slot[3], slot[2]}})
    {
      const float area = anchor_v5_area(initial[tri[0]], initial[tri[1]], initial[tri[2]]);
      if (fabsf(area) > 1.0e-14f) {
        triangles.push_back({tri[0], tri[1], tri[2], area > 0.0f ? 1.0f : -1.0f});
      }
    }
  }
  const auto energy = [&](const std::vector<float2> &coords) {
    double value = 0.0;
    for (const auto &edge : edges) {
      const double strain = double(math::length(coords[edge.a] - coords[edge.b])) /
                            double(edge.length) - 1.0;
      value += strain * strain;
    }
    return value;
  };
  path.metric_before = energy(uv);
  path.metric_after = path.metric_before;
  if (!std::isfinite(path.metric_before) || path.metric_before <= 1.0e-12 * edges.size()) {
    return; /* A regular/flat map needs no iterative solve or solver work arrays. */
  }
  const double radius2 = double(path.coverage_radius) * path.coverage_radius;
  const auto objective = [&](const std::vector<float2> &coords) {
    double value = energy(coords);
    for (size_t i = 0; i < coords.size(); ++i) {
      const float2 delta = coords[i] - initial[i];
      value += 0.02 * double(delta.x * delta.x + delta.y * delta.y) /
               std::max(radius2, 1.0e-20);
    }
    return value;
  };
  double best = objective(uv);
  std::vector<float2> proposed(uv.size()), candidate(uv.size()), sums(uv.size());
  std::vector<double> weights(uv.size());
  for (int iteration = 0; iteration < 5; ++iteration) {
    std::fill(sums.begin(), sums.end(), float2(0.0f));
    std::fill(weights.begin(), weights.end(), 0.0);
    for (const auto &edge : edges) {
      const float2 delta = uv[edge.a] - uv[edge.b];
      const float length = math::length(delta);
      if (length < 1.0e-12f) { continue; }
      const float2 target = (edge.length / length) * delta;
      const double weight = edge.weight;
      sums[edge.a] = sums[edge.a] + float(weight) * (uv[edge.b] + target);
      sums[edge.b] = sums[edge.b] + float(weight) * (uv[edge.a] - target);
      weights[edge.a] += weight;
      weights[edge.b] += weight;
    }
    for (size_t i = 0; i < uv.size(); ++i) {
      const double tether = 0.02 / std::max(radius2, 1.0e-20);
      const bool pinned = indices[i] == fixed_seed ||
          std::find(path.root_controls.begin(),path.root_controls.end(),indices[i]) != path.root_controls.end();
      proposed[i] = pinned ? initial[i] :
          (1.0f / float(weights[i] + tether)) * (sums[i] + float(tether) * initial[i]);
    }
    bool accepted = false;
    for (float step = 0.5f; step >= 0.03125f; step *= 0.5f) {
      for (size_t i = 0; i < uv.size(); ++i) {
        candidate[i] = uv[i] + step * (proposed[i] - uv[i]);
      }
      bool valid = true;
      for (const auto &tri : triangles) {
        if (tri.sign * anchor_v5_area(candidate[tri.a], candidate[tri.b], candidate[tri.c])
            <= 1.0e-14f) { valid = false; break; }
      }
      const double next = valid ? objective(candidate) : best;
      if (valid && std::isfinite(next) && next < best) {
        best = next;
        uv.swap(candidate);
        accepted = true;
        ++path.relax_iterations;
        break;
      }
    }
    if (!accepted) { break; }
  }
  path.metric_after = energy(uv);
  /* A lower tethered objective can trade away metric error. Keep V4 initialization
   * unless metric error itself also improves. Both sets are always finite here. */
  if (path.metric_after > path.metric_before) {
    uv = initial;
    path.metric_after = path.metric_before;
    path.relax_iterations = 0;
  }
  for (size_t i = 0; i < indices.size(); ++i) { path.frames.at(indices[i]).uv = uv[i]; }
}

static bool anchor_quality_v52_enabled()
{
  static const bool enabled = []() {
    const char *value = std::getenv("BLENDER_ANCHOR_PROJECTION_V52");
    return !(value && value[0] == '0');
  }();
  return enabled;
}

struct AnchorV52Source {
  int grid = -1;
  float u = 0.0f, v = 0.0f;
  float3 position = float3(0.0f), normal = float3(0.0f);
  float error = 0.0f;
};

/* Local CCG bilinear evaluation: no mesh-wide search or spatial welding. */
static bool anchor_v52_point(const SubdivCCG &ccg, const CCGKey &key,
                             const int grid, const float u, const float v,
                             float3 &position, float3 *normal = nullptr,
                             float3 *du = nullptr, float3 *dv = nullptr)
{
  if (grid < 0 || grid >= ccg.grids_num || !std::isfinite(u) || !std::isfinite(v)) {
    return false;
  }
  const int x = std::min(std::max(int(u), 0), key.grid_size - 2);
  const int y = std::min(std::max(int(v), 0), key.grid_size - 2);
  const float tx = std::min(std::max(u - x, 0.0f), 1.0f);
  const float ty = std::min(std::max(v - y, 0.0f), 1.0f);
  const int i = grid * key.grid_area + y * key.grid_size + x;
  const auto p = ccg.positions.as_span();
  const float3 &p00 = p[i], &p10 = p[i+1], &p01 = p[i+key.grid_size], &p11 = p[i+key.grid_size+1];
  position = (1.0f-ty)*((1.0f-tx)*p00 + tx*p10) + ty*((1.0f-tx)*p01 + tx*p11);
  if (du) { *du = (1.0f-ty)*(p10-p00) + ty*(p11-p01); }
  if (dv) { *dv = (1.0f-tx)*(p01-p00) + tx*(p11-p10); }
  if (normal) {
    const auto n = ccg.normals.as_span();
    *normal = (1.0f-ty)*((1.0f-tx)*n[i] + tx*n[i+1]) +
               ty*((1.0f-tx)*n[i+key.grid_size] + tx*n[i+key.grid_size+1]);
    if (!anchor_safe_normalize(*normal)) { return false; }
  }
  return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

/* Find the continuous anchor on the hit grid and its topological duplicates.
 * At a seam this avoids choosing a grid whose domain lies on the wrong side. */
static bool anchor_v52_source(const SubdivCCG &ccg, const CCGKey &key,
                              const SubdivCCGCoord &hit, const float3 &origin,
                              AnchorV52Source &source)
{
  const auto fit = [&](const SubdivCCGCoord &candidate) {
    float u = candidate.x, v = candidate.y;
    float3 p, du, dv;
    for (int iteration = 0; iteration < 8; ++iteration) {
      if (!anchor_v52_point(ccg, key, candidate.grid_index, u, v, p, nullptr, &du, &dv)) { return; }
      const float3 delta = origin - p;
      const double a = math::dot(du,du), b = math::dot(du,dv), c = math::dot(dv,dv);
      const double determinant = a*c - b*b;
      if (!(a > 0.0 && c > 0.0 && determinant > 1.0e-8*a*c)) { return; }
      const double ru = math::dot(delta,du), rv = math::dot(delta,dv);
      const float step_u = float((c*ru - b*rv) / determinant);
      const float step_v = float((a*rv - b*ru) / determinant);
      if (!std::isfinite(step_u) || !std::isfinite(step_v)) { return; }
      const float next_u = std::min(std::max(u + std::min(std::max(step_u,-1.0f),1.0f),0.0f),float(key.grid_size-1));
      const float next_v = std::min(std::max(v + std::min(std::max(step_v,-1.0f),1.0f),0.0f),float(key.grid_size-1));
      const float change = fabsf(next_u-u) + fabsf(next_v-v);
      u = next_u; v = next_v;
      if (change < 1.0e-5f) { break; }
    }
    float3 normal;
    if (!anchor_v52_point(ccg, key, candidate.grid_index, u, v, p, &normal)) { return; }
    const float error = math::distance(p,origin);
    if (!std::isfinite(error)) { return; }
    if (source.grid < 0 || error < source.error) {
      source.grid = candidate.grid_index; source.u = u; source.v = v;
      source.position = p; source.normal = normal; source.error = error;
    }
  };
  fit(hit);
  if (hit.x == 0 || hit.y == 0 || hit.x == key.grid_size-1 || hit.y == key.grid_size-1) {
    SubdivCCGNeighbors neighbors;
    BKE_subdiv_ccg_neighbor_coords_get(ccg, hit, true, neighbors);
    for (const auto &duplicate : neighbors.duplicates()) { fit(duplicate); }
  }
  return source.grid >= 0;
}

/* Probe inside each visited coarse cell too. Edge-only tests can miss a fold
 * that never intersects a cell edge or diagonal. Probe count is fixed per cell,
 * not a visited-sample cap. Features between probes can still be missed. */
static bool anchor_v52_cell_error(const SubdivCCG &ccg, const CCGKey &key,
                                  const SubdivCCGCoord &coord, const int stride,
                                  const float tolerance)
{
  if (stride <= 1 || coord.x+stride >= key.grid_size || coord.y+stride >= key.grid_size) {
    return false;
  }
  const auto p = ccg.positions.as_span();
  const int i = coord.to_index(key);
  const float3 &p00=p[i], &p10=p[i+stride], &p01=p[i+stride*key.grid_size],
               &p11=p[i+stride*(key.grid_size+1)];
  for (int py=1; py<=3; ++py) for (int px=1; px<=3; ++px) {
    const int x=(px*stride)/4, y=(py*stride)/4;
    if (x==0 || y==0 || x==stride || y==stride) { continue; }
    const float tx=float(x)/stride, ty=float(y)/stride;
    const float3 predicted=(1.0f-ty)*((1.0f-tx)*p00+tx*p10)+ty*((1.0f-tx)*p01+tx*p11);
    const float error=math::distance(predicted,p[i+y*key.grid_size+x]);
    if (!std::isfinite(error) || error>tolerance) { return true; }
  }
  return false;
}

static bool anchor_v52_seed_frame(const SubdivCCG &ccg, const CCGKey &key,
                                  const AnchorV52Source &source,
                                  const SubdivCCGCoord &control, const float3 &axis,
                                  const float handedness, AnchorSurfacePathFrame &frame,
                                  float &distance)
{
  const float delta_u=control.x-source.u, delta_v=control.y-source.v;
  const int steps=std::max(1,int(std::ceil(std::max(fabsf(delta_u),fabsf(delta_v)))));
  float3 previous=source.position, previous_normal=source.normal, previous_axis=axis;
  frame.uv=float2(0.0f); distance=0.0f;
  for (int i=1; i<=steps; ++i) {
    const float t=float(i)/steps;
    float3 p,n,next_axis;
    if (!anchor_v52_point(ccg,key,source.grid,source.u+t*delta_u,source.v+t*delta_v,p,&n) ||
        !anchor_transport_tangent_x(previous_axis,previous_normal,n,next_axis)) { return false; }
    const float3 delta=p-previous;
    const float length=math::distance(p,previous);
    const float3 y0=handedness*math::cross(previous_normal,previous_axis), y1=handedness*math::cross(n,next_axis);
    float2 uv_delta={0.5f*(math::dot(delta,previous_axis)+math::dot(delta,next_axis)),
                     0.5f*(math::dot(delta,y0)+math::dot(delta,y1))};
    const float projected=math::length(uv_delta);
    if (projected>1.0e-12f) { uv_delta=(length/projected)*uv_delta; }
    else if (length>1.0e-12f) { return false; }
    frame.uv=frame.uv+uv_delta; distance+=length;
    previous=p; previous_normal=n; previous_axis=next_axis;
  }
  frame.tangent_x=previous_axis;
  return std::isfinite(distance) && std::isfinite(frame.uv.x) && std::isfinite(frame.uv.y);
}

static bool anchor_v5_build(const SubdivCCG &ccg,
                            const CCGKey &key,
                            const SubdivCCGCoord &hit,
                            const StrokeCache &stroke,
                            const float3 &reference_axis,
                            const float3 &reference_y,
                            AnchorSurfacePathCache &path,
                            AnchorProjectionDiagnostics *diag)
{
  const Span<float3> positions = ccg.positions.as_span();
  const Span<float3> normals = ccg.normals.as_span();
  if (normals.size() != positions.size()) { return false; }
  const bool quality52 = anchor_quality_v52_enabled();
  AnchorV52Source source;
  const bool virtual_source = quality52 &&
      anchor_v52_source(ccg, key, hit, stroke.location_symm, source) &&
      source.error <= stroke.radius * 0.01f;
  /* A remote symmetry/tile origin must not be dragged onto a nearby surface. */
  float3 n = virtual_source ? source.normal : normals[hit.to_index(key)];
  if (!anchor_safe_normalize(n)) { return false; }
  float3 axis = reference_axis - math::dot(reference_axis, n) * n;
  if (!anchor_safe_normalize(axis)) { return false; }
  if (!std::isfinite(reference_y.x) || !std::isfinite(reference_y.y) ||
      !std::isfinite(reference_y.z)) { return false; }
  path.reference_axis = axis;
  path.reference_normal = n;
  path.handedness = math::dot(math::cross(n, axis), reference_y) < 0.0f ? -1.0f : 1.0f;
  path.grid_size = key.grid_size;
  path.grid_area = key.grid_area;
  path.position_count = positions.size(); path.normal_count = normals.size();
  path.position_buffer = positions.data(); path.normal_buffer = normals.data();
  path.projection_v5 = true;
  path.projection_v52 = quality52;
  path.source_grid = virtual_source ? source.grid : -1;
  path.source_u = source.u; path.source_v = source.v; path.source_error = source.error;
  /* Choose spacing from local geometry, not a vertex-count cutoff. Power-of-two
   * strides divide the CCG lattice and remain aligned at every seam. */
  float spacing = 0.0f;
  const int hit_index = hit.to_index(key);
  for (const auto &delta : {std::array<int, 2>{1, 0}, std::array<int, 2>{0, 1}}) {
    const int x = hit.x + (hit.x < key.grid_size - 1 ? delta[0] : -delta[0]);
    const int y = hit.y + (hit.y < key.grid_size - 1 ? delta[1] : -delta[1]);
    const float edge = math::distance(positions[hit_index],
        positions[hit.grid_index * key.grid_area + y * key.grid_size + x]);
    if (edge > 1.0e-12f) { spacing = spacing == 0.0f ? edge : std::min(spacing, edge); }
  }
  int stride = 1;
  while (spacing > 0.0f && stride * 2 <= key.grid_size - 1 &&
         (key.grid_size - 1) % (stride * 2) == 0 &&
         float(stride * 2) * spacing < stroke.radius / 64.0f)
  { stride *= 2; }
  if (quality52 && path.preferred_stride > 0 && stride >= path.preferred_stride &&
      stride < path.preferred_stride * 4 && (key.grid_size-1) % path.preferred_stride == 0)
  { stride = path.preferred_stride; } /* Hysteresis avoids regridding at each 2x threshold. */
  if (!ccg.grid_hidden.is_empty()) { stride = 1; }
  struct Candidate {
    float distance;
    int index;
    bool operator>(const Candidate &other) const {
      return distance == other.distance ? index > other.index : distance > other.distance;
    }
  };
  for (;;) {
    path.stride = stride;
    path.last_control = (key.grid_size - 1) / stride;
    path.stride_shift = 0;
    for (int step = stride; step > 1; step >>= 1) { ++path.stride_shift; }
    path.inverse_stride = 1.0f / stride;
    path.blocks_per_row = (path.last_control + 8) / 8;
    path.aliases.clear(); path.distances.clear(); path.frames.clear();
    std::vector<AnchorV5MetricEdge> edges;
    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> queue;
    int seed_index = -1;
    path.root_controls = {-1, -1, -1, -1};
    path.seed_count = 0;
    if (virtual_source) {
      const int x0=(int(source.u)/stride)*stride, y0=(int(source.v)/stride)*stride;
      const int x1=std::min(x0+stride,key.grid_size-1), y1=std::min(y0+stride,key.grid_size-1);
      const int xs[4]={x0,x1,x0,x1}, ys[4]={y0,y0,y1,y1};
      bool failed_seed = false;
      float closest = path.coverage_radius;
      for (int i=0; i<4; ++i) {
        SubdivCCGCoord control{}; control.grid_index=source.grid;
        control.x=short(xs[i]); control.y=short(ys[i]);
        const int raw=control.to_index(key);
        if (!ccg.grid_hidden.is_empty() && ccg.grid_hidden[source.grid][ys[i]*key.grid_size+xs[i]]) {
          continue;
        }
        const int canonical=anchor_v5_canonical(ccg,key,path,raw);
        path.root_controls[i]=canonical;
        if (path.distances.count(canonical)) { continue; }
        AnchorSurfacePathFrame frame; float distance;
        if (!anchor_v52_seed_frame(ccg,key,source,control,axis,path.handedness,frame,distance)) {
          failed_seed=true; continue;
        }
        if (distance>path.coverage_radius) { continue; }
        path.frames.emplace(canonical,frame); path.distances.emplace(canonical,distance);
        queue.push({distance,canonical}); ++path.seed_count;
        if (seed_index<0 || distance<closest) { seed_index=canonical; closest=distance; }
      }
      if (failed_seed && stride>1) { stride/=2; ++path.refinements; continue; }
      if (seed_index<0) { return false; }
    }
    else {
      SubdivCCGCoord seed = hit;
      seed.x = short(std::min(((int(hit.x) + stride / 2) / stride) * stride, key.grid_size - 1));
      seed.y = short(std::min(((int(hit.y) + stride / 2) / stride) * stride, key.grid_size - 1));
      seed_index = anchor_v5_canonical(ccg, key, path, seed.to_index(key));
      float3 seed_n = normals[seed_index];
      if (!anchor_safe_normalize(seed_n)) {
        if (stride > 1) { stride /= 2; ++path.refinements; continue; }
        return false;
      }
      float3 seed_axis;
      if (!anchor_transport_tangent_x(axis, n, seed_n, seed_axis)) {
        if (stride > 1) { stride /= 2; ++path.refinements; continue; }
        return false;
      }
      const float3 seed_y = path.handedness * math::cross(seed_n, seed_axis);
      const float3 offset = positions[seed_index] - stroke.location_symm;
      AnchorSurfacePathFrame seed_frame;
      seed_frame.tangent_x = seed_axis;
      seed_frame.uv = {math::dot(offset, seed_axis), math::dot(offset, seed_y)};
      if (!std::isfinite(seed_frame.uv.x) || !std::isfinite(seed_frame.uv.y) ||
          !std::isfinite(math::distance(positions[seed_index], stroke.location_symm))) {
        return false;
      }
      path.frames.emplace(seed_index, seed_frame);
      /* Seed UV contains the real hit offset. The radial label accounts for it too. */
      path.distances.emplace(seed_index, math::distance(positions[seed_index], stroke.location_symm));
      queue.push({path.distances.at(seed_index), seed_index});
      path.seed_count = 1;
    }
    bool refine = false;
    const float cutoff = path.coverage_radius;
    while (!queue.empty() && !refine) {
      const Candidate item = queue.top(); queue.pop();
      if (diag) { ++diag->popped; }
      if (item.distance > path.distances.at(item.index)) { continue; }
      const auto expand = [&](const SubdivCCGCoord &coord) {
        if (quality52 && anchor_v52_cell_error(ccg,key,coord,stride,stroke.radius*0.001f)) {
          refine=true; return;
        }
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) { continue; }
          const int nx = coord.x + dx * stride, ny = coord.y + dy * stride;
          if (nx < 0 || ny < 0 || nx >= key.grid_size || ny >= key.grid_size) { continue; }
          const int raw_next = coord.grid_index * key.grid_area + ny * key.grid_size + nx;
          if (!ccg.grid_hidden.is_empty() && ccg.grid_hidden[coord.grid_index][ny * key.grid_size + nx]) { continue; }
          const int next = anchor_v5_canonical(ccg, key, path, raw_next);
          if (next == item.index) { continue; }
          /* Integrate actual high-res edge samples: a coarse chord must not jump
           * through a concavity. This reads only edges in the local stamp. */
          float length = 0.0f;
          float3 previous = positions[coord.to_index(key)];
          for (int step = 1; step <= stride; ++step) {
            const int sample = coord.grid_index * key.grid_area +
                (coord.y + dy * step) * key.grid_size + coord.x + dx * step;
            length += math::distance(previous, positions[sample]); previous = positions[sample];
          }
          if (!std::isfinite(length)) { continue; }
          const float cost = item.distance + length;
          if (cost > cutoff) { continue; }
          if (item.index < next) { edges.push_back({item.index, next, length}); }
          const float chord = math::distance(positions[item.index], positions[next]);
          if (stride > 1 && length - chord > std::max(stroke.radius * 0.001f, chord * 0.015f)) {
            refine = true;
            return; /* This coarse attempt will be discarded: stop immediately. */
          }
          const auto best = path.distances.find(next);
          if (best != path.distances.end() && cost >= best->second) { continue; }
          if (diag) {
            ++diag->relaxed;
            diag->seam_edges += size_t(coord.grid_index != item.index / key.grid_area);
          }
          const auto parent = path.frames.find(item.index);
          float3 next_axis;
          if (parent != path.frames.end() && anchor_transport_tangent_x(
              parent->second.tangent_x, normals[item.index], normals[next], next_axis))
          {
            float3 n0 = normals[item.index], n1 = normals[next];
            normalize_v3(n0); normalize_v3(n1);
            const float3 y0 = path.handedness * math::cross(n0, parent->second.tangent_x);
            const float3 y1 = path.handedness * math::cross(n1, next_axis);
            const float3 delta = positions[next] - positions[item.index];
            float2 uv_delta = {0.5f * (math::dot(delta, parent->second.tangent_x) + math::dot(delta, next_axis)),
                              0.5f * (math::dot(delta, y0) + math::dot(delta, y1))};
            const float projected_length = math::length(uv_delta);
            if (projected_length > 1.0e-12f) { uv_delta = (length / projected_length) * uv_delta; }
            AnchorSurfacePathFrame frame; frame.tangent_x = next_axis;
            frame.uv = parent->second.uv + uv_delta;
            if (std::isfinite(frame.uv.x) && std::isfinite(frame.uv.y)) {
              path.frames.insert_or_assign(next, frame);
            }
            else { path.frames.erase(next); if (diag) { ++diag->nonfinite_uv; } }
          }
          else {
            path.frames.erase(next);
            if (diag) { ++diag->failed_transport; }
            if (stride > 1) { refine = true; return; }
          }
          path.distances.insert_or_assign(next, cost);
          queue.push({cost, next});
        }
      };
      const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, item.index);
      expand(coord);
      if (!refine && (coord.x == 0 || coord.y == 0 || coord.x == key.grid_size - 1 || coord.y == key.grid_size - 1)) {
        SubdivCCGNeighbors neighbors;
        BKE_subdiv_ccg_neighbor_coords_get(ccg, coord, true, neighbors);
        for (const auto &duplicate : neighbors.duplicates()) { expand(duplicate); if (refine) { break; } }
      }
    }
    if (refine && stride > 1) { stride /= 2; ++path.refinements; continue; }
    std::sort(edges.begin(), edges.end(), [](const auto &a, const auto &b) {
      if (a.a != b.a) { return a.a < b.a; }
      if (a.b != b.b) { return a.b < b.b; }
      return a.length < b.length;
    });
    edges.erase(std::unique(edges.begin(), edges.end(), [](const auto &a, const auto &b) {
      return a.a == b.a && a.b == b.b;
    }), edges.end());
    const double relax_start = diag ? BLI_time_now_seconds() : 0.0;
    anchor_v5_relax(path, seed_index, edges);
    path.relax_ms = diag ? (BLI_time_now_seconds() - relax_start) * 1000.0 : 0.0;
    path.radial_correction = false;
    if (virtual_source) {
      const int x0=(int(source.u)/stride)*stride, y0=(int(source.v)/stride)*stride;
      const int x1=std::min(x0+stride,key.grid_size-1), y1=std::min(y0+stride,key.grid_size-1);
      const float tx=x1==x0?0.0f:(source.u-x0)/float(x1-x0);
      const float ty=y1==y0?0.0f:(source.v-y0)/float(y1-y0);
      const float weights[4]={(1-tx)*(1-ty),tx*(1-ty),(1-tx)*ty,tx*ty};
      float2 center(0.0f); bool valid=true;
      for (int i=0; i<4; ++i) {
        if (weights[i]==0.0f) { continue; }
        const auto frame=path.frames.find(path.root_controls[i]);
        if (frame==path.frames.end()) { valid=false; break; }
        center=center+weights[i]*frame->second.uv;
      }
      if (valid && std::isfinite(center.x) && std::isfinite(center.y)) {
        for (auto &frame : path.frames) { frame.second.uv=frame.second.uv-center; }
        path.radial_correction=true;
      }
    }
    path.control_blocks.clear();
    for (const auto &alias : path.aliases) {
      const auto distance = path.distances.find(alias.second);
      if (distance == path.distances.end()) { continue; }
      AnchorV5Sample sample; sample.distance = distance->second;
      const auto frame = path.frames.find(alias.second);
      sample.has_uv = frame != path.frames.end();
      if (sample.has_uv) { sample.uv = frame->second.uv; }
      if (!std::isfinite(sample.distance)) { continue; }
      sample.has_uv = sample.has_uv && std::isfinite(sample.uv.x) && std::isfinite(sample.uv.y);
      if (path.radial_correction && sample.has_uv) {
        /* Interpolate radial residual, not radius itself: sqrt(U^2+V^2) restores
         * the continuous center. Root-cell controls have zero local residual. */
        const bool root=std::find(path.root_controls.begin(),path.root_controls.end(),alias.second)!=path.root_controls.end();
        sample.distance=root?0.0f:sample.distance-math::length(sample.uv);
      }
      const int grid = alias.first / key.grid_area, local = alias.first % key.grid_area;
      const int cx = (local % key.grid_size) / stride, cy = (local / key.grid_size) / stride;
      auto &block = path.control_blocks[anchor_v51_block_key(path, grid, cx, cy)];
      const int slot = (cy % 8) * 8 + cx % 8;
      block.values[slot] = sample;
      block.present |= uint64_t(1) << slot;
    }
    if (diag && diag->level >= 2) {
      const double audit_start = BLI_time_now_seconds();
      for (const auto &alias : path.aliases) {
        if (alias.first == alias.second) { continue; }
        const auto *raw = anchor_v51_control(path, alias.first);
        const auto *canonical = anchor_v51_control(path, alias.second);
        if (!raw || !canonical) { continue; }
        if (!raw->has_uv || !canonical->has_uv) { ++diag->seam_missing; }
        else {
          ++diag->seam_pairs;
          diag->seam_uv_max = std::max(diag->seam_uv_max,
              math::length(raw->uv - canonical->uv));
        }
      }
      for (const auto &frame : path.frames) {
        float3 normal = normals[frame.first];
        if (normalize_v3(normal) > 1.0e-6f) {
          diag->tangent_error_max = std::max(diag->tangent_error_max,
              fabsf(math::dot(normal, frame.second.tangent_x)));
        }
      }
      diag->audit_ms = (BLI_time_now_seconds() - audit_start) * 1000.0;
    }
    if (diag) {
      float3 expected_normal = stroke.sculpt_normal_symm;
      normalize_v3(expected_normal);
      diag->seed_normal_dot = math::dot(path.reference_normal, expected_normal);
      float3 expected_y = reference_y; normalize_v3(expected_y);
      diag->seed_y_dot = math::dot(path.handedness * math::cross(n, axis), expected_y);
    }
    path.transport_ready = true;
    path.ready = true;
    return true;
  }
}

/* ANCHORED_GEODESIC_V3: local Dijkstra over the actual Multires CCG surface.
 * This cache is built before worker threads and accessed read-only by them.
 * Radial distance follows grid edges and cross-face seams; angular coordinates
 * still follow the brush tangent plane (this is not a complete 2D unwrap). */
static void anchor_surface_path_prepare(const Brush &brush, Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  cache.anchor_projection_diagnostics.reset();
  if (cache.anchor_surface_allocation_failed) {
    cache.anchor_surface_path.reset();
    return; /* Avoid repeating a failed large allocation on every mouse event. */
  }
  if (anchor_diagnostic_level() && brush.stroke_method == BRUSH_STROKE_ANCHORED) {
    cache.anchor_projection_diagnostics = std::make_shared<AnchorProjectionDiagnostics>();
    cache.anchor_projection_diagnostics->level = anchor_diagnostic_level();
    cache.anchor_projection_diagnostics->requested =
        bool(brush.flag2 & BRUSH_SCULPT_ANCHOR_TRANSPORT);
  }
  auto *diag = cache.anchor_projection_diagnostics.get();
  const AnchorDiagnosticTimer timer{diag, diag ? BLI_time_now_seconds() : 0.0};
  if (!ss.subdiv_ccg || !mtex->tex || mtex->brush_map_mode != MTEX_MAP_MODE_AREA ||
      brush.stroke_method != BRUSH_STROKE_ANCHORED ||
      !(brush.flag2 & BRUSH_SCULPT_ANCHOR_SURFACE_PROJECT) ||
      !(brush.flag2 & BRUSH_SCULPT_ANCHOR_GEODESIC) || cache.radius <= 1.0e-6f || !std::isfinite(cache.radius * 1.35f))
  {
    if (diag) {
      diag->reason = !ss.subdiv_ccg ? "no_ccg" : !mtex->tex ? "no_texture" :
          mtex->brush_map_mode != MTEX_MAP_MODE_AREA ? "non_area_mapping" :
          !(brush.flag2 & BRUSH_SCULPT_ANCHOR_SURFACE_PROJECT) ? "surface_disabled" :
          !(brush.flag2 & BRUSH_SCULPT_ANCHOR_GEODESIC) ? "geodesic_disabled" : "tiny_radius";
    }
    cache.anchor_surface_path.reset();
    cache.anchor_surface_passes.clear();
    return;
  }

  const SubdivCCG &ccg = *ss.subdiv_ccg;
  const float cutoff = cache.radius * 1.35f;
  const CCGKey layout = BKE_subdiv_ccg_key_top_level(ccg);
  const bool v5_enabled = anchor_projection_v5_enabled();
  const bool transport_requested =
      (brush.flag2 & BRUSH_SCULPT_ANCHOR_TRANSPORT) &&
      (v5_enabled || (cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0 &&
                     cache.tile_pass == 0));
  const bool wants_v5 = v5_enabled && transport_requested;
  if (wants_v5) {
    cache.anchor_surface_path.reset();
    for (const auto &previous : cache.anchor_surface_passes) {
      if (previous->mirror_pass == int(cache.mirror_symmetry_pass) &&
          previous->radial_pass == cache.radial_symmetry_pass &&
          previous->tile_pass == cache.tile_pass)
      { cache.anchor_surface_path = previous; break; }
    }
  }
  if (diag) {
    diag->transport_reason = !diag->requested ? "disabled" :
        !transport_requested ? "symmetry_or_tile" : "eligible";
  }
  float3 reference_axis = {cache.brush_local_mat_inv[0][0],
                           cache.brush_local_mat_inv[0][1],
                           cache.brush_local_mat_inv[0][2]};
  float3 reference_y = {cache.brush_local_mat_inv[1][0],
                        cache.brush_local_mat_inv[1][1],
                        cache.brush_local_mat_inv[1][2]};
  if (wants_v5) {
    reference_axis = symmetry_flip(reference_axis, cache.mirror_symmetry_pass);
    reference_y = symmetry_flip(reference_y, cache.mirror_symmetry_pass);
    mul_mat3_m4_v3(cache.symm_rot_mat.ptr(), reference_axis);
    mul_mat3_m4_v3(cache.symm_rot_mat.ptr(), reference_y);
  }
  const bool reference_valid = normalize_v3(reference_axis) > 1.0e-6f;
  if (diag && cache.anchor_surface_path) {
    const auto &previous = *cache.anchor_surface_path;
    diag->cache_event = previous.geometry != &ccg ? "geometry_changed" :
        previous.grid_size != layout.grid_size || previous.grid_area != layout.grid_area ||
        previous.position_count != ccg.positions.as_span().size() ||
        previous.normal_count != ccg.normals.as_span().size() ||
        previous.position_buffer != ccg.positions.as_span().data() ||
        previous.normal_buffer != ccg.normals.as_span().data() ? "layout_changed" :
        previous.coverage_radius < cache.radius * 1.05f ? "radius_growth" :
        previous.mirror_pass != int(cache.mirror_symmetry_pass) ||
        previous.radial_pass != cache.radial_symmetry_pass ||
        previous.tile_pass != cache.tile_pass ? "pass_changed" :
        math::distance_squared(previous.origin, cache.location_symm) >= 1.0e-12f ?
            "origin_changed" :
        transport_requested && !previous.transport_ready ? "transport_not_ready" :
        transport_requested && (!reference_valid || (!previous.projection_v5 &&
            math::dot(previous.reference_axis, reference_axis) <= 0.9999f)) ?
            "axis_changed" : "reusable";
  }
  if (cache.anchor_surface_path && cache.anchor_surface_path->ready &&
      cache.anchor_surface_path->geometry == &ccg &&
      cache.anchor_surface_path->grid_size == layout.grid_size &&
      cache.anchor_surface_path->grid_area == layout.grid_area &&
      cache.anchor_surface_path->position_count == ccg.positions.as_span().size() &&
      cache.anchor_surface_path->normal_count == ccg.normals.as_span().size() &&
      cache.anchor_surface_path->position_buffer == ccg.positions.as_span().data() &&
      cache.anchor_surface_path->normal_buffer == ccg.normals.as_span().data() &&
      cache.anchor_surface_path->coverage_radius >= cache.radius * 1.05f &&
      cache.anchor_surface_path->mirror_pass == int(cache.mirror_symmetry_pass) &&
      cache.anchor_surface_path->radial_pass == cache.radial_symmetry_pass &&
      cache.anchor_surface_path->tile_pass == cache.tile_pass &&
      cache.anchor_surface_path->projection_v5 == wants_v5 &&
      (!transport_requested ||
       (reference_valid && cache.anchor_surface_path->transport_ready &&
        (wants_v5 ? anchor_v5_set_rotation(*cache.anchor_surface_path, reference_axis) :
         math::dot(cache.anchor_surface_path->reference_axis, reference_axis) > 0.9999f))) &&
      math::distance_squared(cache.anchor_surface_path->origin, cache.location_symm) <
          1.0e-12f)
  {
    if (diag) {
      diag->reason = "reuse";
      diag->visited = cache.anchor_surface_path->distances.size();
      diag->frames = cache.anchor_surface_path->frames.size();
      diag->built_transport = cache.anchor_surface_path->transport_ready;
      diag->stride = cache.anchor_surface_path->stride;
      diag->refinements = cache.anchor_surface_path->refinements;
      diag->relax_iterations = cache.anchor_surface_path->relax_iterations;
      diag->metric_before = cache.anchor_surface_path->metric_before;
      diag->metric_after = cache.anchor_surface_path->metric_after;
      diag->seed_count=cache.anchor_surface_path->seed_count;
      diag->source_error=cache.anchor_surface_path->source_error;
    }
    return;
  }

  int preferred_stride=0;
  if (wants_v5 && cache.anchor_surface_path && cache.anchor_surface_path->projection_v5 &&
      cache.anchor_surface_path->geometry==&ccg &&
      cache.anchor_surface_path->grid_size==layout.grid_size &&
      cache.anchor_surface_path->position_buffer==ccg.positions.as_span().data() &&
      math::distance_squared(cache.anchor_surface_path->origin,cache.location_symm)<1.0e-12f)
  { preferred_stride=cache.anchor_surface_path->stride; }
  cache.anchor_surface_path.reset();
  const CCGKey key = BKE_subdiv_ccg_key_top_level(ccg);
  const Span<float3> positions = ccg.positions.as_span();
  if (key.grid_size <= 1 || key.grid_area <= 0 || ccg.grids_num <= 0 ||
      positions.size() != int64_t(ccg.grids_num) * key.grid_area)
  {
    if (diag) { diag->reason = "invalid_ccg"; }
    return;
  }
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const std::optional<SubdivCCGCoord> seed = nearest_vert_calc_grids(
      pbvh, ccg, cache.location_symm, std::max(cache.radius, 1.0e-4f), false);
  if (!seed || !BKE_subdiv_ccg_check_coord_valid(ccg, *seed)) {
    if (diag) { diag->reason = "no_seed"; }
    return;
  }

  auto path = std::make_shared<AnchorSurfacePathCache>();
  path->geometry = &ccg;
  path->origin = cache.location_symm;
  path->coverage_radius = cutoff;
  path->mirror_pass = int(cache.mirror_symmetry_pass);
  path->radial_pass = cache.radial_symmetry_pass;
  path->tile_pass = cache.tile_pass;
  path->grid_size = key.grid_size; path->grid_area = key.grid_area;
  path->position_count = positions.size(); path->normal_count = ccg.normals.as_span().size();
  path->position_buffer = positions.data(); path->normal_buffer = ccg.normals.as_span().data();

  path->preferred_stride=preferred_stride;
  if (wants_v5 && reference_valid) {
    const double v5_start = diag ? BLI_time_now_seconds() : 0.0;
    if (anchor_v5_build(ccg, key, *seed, cache, reference_axis, reference_y, *path, diag)) {
      bool replaced = false;
      for (auto &previous : cache.anchor_surface_passes) {
        if (previous->mirror_pass == path->mirror_pass && previous->radial_pass == path->radial_pass &&
            previous->tile_pass == path->tile_pass)
        { previous = path; replaced = true; break; }
      }
      if (!replaced) { cache.anchor_surface_passes.append(path); }
      cache.anchor_surface_path = path;
      if (diag) {
        diag->reason = path->projection_v52?"built_v52":"built_v5";
        diag->transport_reason = diag->reason;
        diag->seed_count=path->seed_count; diag->source_error=path->source_error;
        diag->built_transport = true;
        diag->relax_ms = path->relax_ms;
        diag->search_ms = std::max(0.0, (BLI_time_now_seconds() - v5_start) * 1000.0 -
                                  diag->relax_ms - diag->audit_ms);
        diag->visited = path->distances.size(); diag->frames = path->frames.size();
        diag->stride = path->stride; diag->refinements = path->refinements;
        diag->relax_iterations = path->relax_iterations;
        diag->metric_before = path->metric_before; diag->metric_after = path->metric_after;
      }
      return;
    }
    /* Unsupported/degenerate frames keep the existing dense V4/V3 fallback.
     * Recreate the empty path so an incomplete adaptive map is never sampled. */
    path = std::make_shared<AnchorSurfacePathCache>();
    path->geometry = &ccg; path->origin = cache.location_symm; path->coverage_radius = cutoff;
    path->mirror_pass = int(cache.mirror_symmetry_pass);
    path->radial_pass = cache.radial_symmetry_pass; path->tile_pass = cache.tile_pass;
    path->grid_size = key.grid_size; path->grid_area = key.grid_area;
    path->position_count = positions.size(); path->normal_count = ccg.normals.as_span().size();
    path->position_buffer = positions.data(); path->normal_buffer = ccg.normals.as_span().data();
  }
  const Span<float3> normals = ccg.normals.as_span();
  bool build_transport = transport_requested && reference_valid &&
                         (!wants_v5 || (cache.mirror_symmetry_pass == 0 &&
                          cache.radial_symmetry_pass == 0 && cache.tile_pass == 0)) &&
                         normals.size() == positions.size();
  if (diag && transport_requested) {
    diag->transport_reason = !reference_valid ? "invalid_reference_axis" :
        normals.size() != positions.size() ? "missing_normals" : "eligible";
  }
  if (build_transport) {
    const float3 &seed_normal = normals[seed->to_index(key)];
    float3 n = seed_normal;
    if (!anchor_safe_normalize(n)) {
      if (diag) { diag->transport_reason = "invalid_seed_normal"; }
      build_transport = false;
    }
    else {
      float3 seed_axis = reference_axis - math::dot(reference_axis, n) * n;
      if (!anchor_safe_normalize(seed_axis)) {
        if (diag) { diag->transport_reason = "degenerate_seed_axis"; }
        build_transport = false;
      }
      else {
        const float3 tangent_y = math::cross(n, seed_axis);
        if (diag) {
          float3 expected_y = {cache.brush_local_mat_inv[1][0],
                               cache.brush_local_mat_inv[1][1],
                               cache.brush_local_mat_inv[1][2]};
          float3 expected_normal = cache.sculpt_normal_symm;
          normalize_v3(expected_y);
          normalize_v3(expected_normal);
          diag->seed_normal_dot = math::dot(n, expected_normal);
          diag->seed_y_dot = math::dot(tangent_y, expected_y);
        }
        const float3 origin_offset = positions[seed->to_index(key)] - cache.location_symm;
        AnchorSurfacePathFrame initial_frame;
        initial_frame.tangent_x = seed_axis;
        initial_frame.uv = {math::dot(origin_offset, seed_axis),
                            math::dot(origin_offset, tangent_y)};
        path->frames.emplace(seed->to_index(key), initial_frame);
        path->reference_axis = reference_axis;
      }
    }
  }
  path->transport_ready = build_transport;
  if (diag) {
    diag->built_transport = build_transport;
    if (build_transport) { diag->transport_reason = "built"; }
  }

  struct Candidate {
    float distance;
    int index;
    bool operator>(const Candidate &other) const { return distance > other.distance; }
  };
  std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> queue;
  /* Uncapped legacy fallback: only the footprint radius bounds traversal. */
  path->distances.reserve(4096);
  const int seed_index = seed->to_index(key);
  path->distances.emplace(seed_index, 0.0f);
  queue.push({0.0f, seed_index});
  const auto &hidden = ccg.grid_hidden;
  const double search_start = diag ? BLI_time_now_seconds() : 0.0;

  while (!queue.empty()) {
    const Candidate item = queue.top();
    queue.pop();
    if (diag) { ++diag->popped; }
    const auto best = path->distances.find(item.index);
    if (best == path->distances.end() || item.distance > best->second) {
      continue;
    }
    if (item.distance > cutoff) {
      break;
    }
    const int grid = item.index / key.grid_area;
    const int in_grid = item.index % key.grid_area;
    const int x = in_grid % key.grid_size;
    const int y = in_grid / key.grid_size;

    const auto try_neighbor = [&](const int next) {
      if (next < 0 || next >= positions.size()) {
        return;
      }
      const int next_grid = next / key.grid_area;
      const int next_local = next % key.grid_area;
      if (!hidden.is_empty() && hidden[next_grid][next_local]) {
        return;
      }
      const float edge = math::distance(positions[item.index], positions[next]);
      if (!std::isfinite(edge)) {
        return;
      }
      const float cost = item.distance + edge;
      if (cost > cutoff) {
        return;
      }
      const auto it = path->distances.find(next);
      if (it != path->distances.end() && cost >= it->second) {
        return;
      }

      if (diag) {
        ++diag->relaxed;
        diag->seam_edges += size_t(next_grid != grid);
      }
      if (build_transport) {
        const auto parent = path->frames.find(item.index);
        float3 transported_axis;
        float3 normal0 = normals[item.index];
        float3 normal1 = normals[next];
        if (parent != path->frames.end() &&
            anchor_transport_tangent_x(
                parent->second.tangent_x, normal0, normal1, transported_axis))
        {
          normalize_v3(normal0);
          normalize_v3(normal1);
          const float3 tangent_y0 = math::cross(normal0, parent->second.tangent_x);
          const float3 tangent_y1 = math::cross(normal1, transported_axis);
          const float3 edge_delta = positions[next] - positions[item.index];
          const float2 uv_delta = {
              0.5f * (math::dot(edge_delta, parent->second.tangent_x) +
                      math::dot(edge_delta, transported_axis)),
              0.5f * (math::dot(edge_delta, tangent_y0) +
                      math::dot(edge_delta, tangent_y1))};
          AnchorSurfacePathFrame frame;
          frame.tangent_x = transported_axis;
          frame.uv = parent->second.uv + uv_delta;
          if (std::isfinite(frame.uv.x) && std::isfinite(frame.uv.y)) {
            path->frames.insert_or_assign(next, frame);
          }
          else {
            if (diag) { ++diag->nonfinite_uv; }
            path->frames.erase(next);
          }
        }
        else {
          if (diag) { ++diag->failed_transport; }
          /* Discontinuous normals fall back to the V3 angular projection. */
          path->frames.erase(next);
        }
      }

      if (it != path->distances.end()) {
        it->second = cost;
      }
      else {
        path->distances.emplace(next, cost);
      }
      queue.push({cost, next});
    };

    /* Eight connected samples inside each quad grid reduce Manhattan artifacts. */
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) {
          continue;
        }
        const int nx = x + dx;
        const int ny = y + dy;
        if (nx >= 0 && ny >= 0 && nx < key.grid_size && ny < key.grid_size) {
          try_neighbor(grid * key.grid_area + ny * key.grid_size + nx);
        }
      }
    }

    /* Follow Blender's cross-grid topology and duplicate CCG seam samples. */
    if (x == 0 || y == 0 || x == key.grid_size - 1 || y == key.grid_size - 1)
    {
      const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, item.index);
      SubdivCCGNeighbors neighbors;
      BKE_subdiv_ccg_neighbor_coords_get(ccg, coord, true, neighbors);
      for (const SubdivCCGCoord &neighbor : neighbors.coords) {
        try_neighbor(neighbor.to_index(key));
      }
    }
  }

  if (diag) {
    diag->search_ms = (BLI_time_now_seconds() - search_start) * 1000.0;
    diag->visited = path->distances.size();
    diag->frames = path->frames.size();
    diag->reason = "built_legacy";
  }
  /* Level 2 audits final duplicate seam coordinates. It never changes the cache. */
  if (diag && diag->level >= 2 && build_transport) {
    const double audit_start = BLI_time_now_seconds();
    for (const auto &entry : path->frames) {
      const int index = entry.first;
      float3 n = normals[index];
      if (normalize_v3(n) > 1.0e-6f) {
        diag->tangent_error_max = std::max(diag->tangent_error_max,
            fabsf(math::dot(n, entry.second.tangent_x)));
      }
      const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, index);
      if (coord.x != 0 && coord.y != 0 && coord.x != key.grid_size - 1 &&
          coord.y != key.grid_size - 1) { continue; }
      SubdivCCGNeighbors neighbors;
      BKE_subdiv_ccg_neighbor_coords_get(ccg, coord, true, neighbors);
      for (const SubdivCCGCoord &neighbor : neighbors.coords) {
        const int next = neighbor.to_index(key);
        if (next <= index || neighbor.grid_index == coord.grid_index ||
            !path->distances.count(next) ||
            math::distance_squared(positions[index], positions[next]) > 1.0e-16f)
        { continue; }
        const auto frame = path->frames.find(next);
        if (frame == path->frames.end()) { ++diag->seam_missing; continue; }
        ++diag->seam_pairs;
        diag->seam_uv_max = std::max(diag->seam_uv_max,
            math::length(entry.second.uv - frame->second.uv));
      }
    }
    diag->audit_ms = (BLI_time_now_seconds() - audit_start) * 1000.0;
  }
  path->ready = true;
  cache.anchor_surface_path = std::move(path);
}

static void do_brush_action(const Depsgraph &depsgraph,
                            const Scene & /*scene*/,
                            Sculpt &sd,
                            Object &ob,
                            const Brush &brush,
                            PaintModeSettings &paint_mode_settings)
{
  PRF_scope(ProfileCategory::Editor);
  PRF_scope_set_dynamic_name("%s", sculpt_brush_type_name(brush));
  SculptSession &ss = *ob.runtime->sculpt_session;
  /* Sculpt Mesh V3 timing probe.
   * Instrumentation only: no sculpt math / PBVH / undo behavior changes. */
  const Mesh *sculpt_mesh_v3_probe_mesh = BKE_object_get_original_mesh(&ob);
  const bool sculpt_mesh_v3_probe_enabled =
      (sculpt_mesh_v3_probe_mesh && sculpt_mesh_v3_probe_mesh->verts_num >= 1'000'000) ||
      (std::getenv("BLENDER_SCULPT_STROKE_PROFILE_LOG") &&
       std::getenv("BLENDER_SCULPT_STROKE_PROFILE_LOG")[0]);
  const double sculpt_mesh_v3_probe_total_start = BLI_time_now_seconds();
  double sculpt_mesh_v3_probe_gather_ms = 0.0;
  double sculpt_mesh_v3_probe_undo_ms = 0.0;
  double sculpt_mesh_v3_probe_prebrush_ms = 0.0;
  int64_t sculpt_mesh_v3_probe_nodes = 0;

  struct SculptMeshV3ProbeScope {
    bool enabled;
    const Brush &brush;
    const int64_t &nodes;
    const double &gather_ms;
    const double &undo_ms;
    const double &prebrush_ms;
    double start;

    ~SculptMeshV3ProbeScope()
    {
      if (!enabled || nodes <= 0) {
        return;
      }
      const double total_ms = (BLI_time_now_seconds() - start) * 1000.0;
      if (total_ms < 0.10) {
        return;
      }
      const double rest_ms = std::max(
          0.0, total_ms - gather_ms - undo_ms - prebrush_ms);
      std::fprintf(stderr,
                   "[SculptMeshV3] brush=%s nodes=%lld total=%.3f ms "
                   "gather=%.3f undo=%.3f prebrush=%.3f brush/rest=%.3f\n",
                   sculpt_brush_type_name(brush),
                   static_cast<long long>(nodes),
                   total_ms,
                   gather_ms,
                   undo_ms,
                   prebrush_ms,
                   rest_ms);
    }
  } sculpt_mesh_v3_probe_scope{
      sculpt_mesh_v3_probe_enabled,
      brush,
      sculpt_mesh_v3_probe_nodes,
      sculpt_mesh_v3_probe_gather_ms,
      sculpt_mesh_v3_probe_undo_ms,
      sculpt_mesh_v3_probe_prebrush_ms,
      sculpt_mesh_v3_probe_total_start};
  const SculptMeshV3MemorySample sculpt_mesh_v3_mem_start = sculpt_mesh_v3_memory_sample();

  struct SculptMeshV3MemoryProbeScope {
    bool enabled;
    const Brush &brush;
    SculptMeshV3MemorySample start_memory;
    double start_time;

    ~SculptMeshV3MemoryProbeScope()
    {
      if (!enabled) {
        return;
      }

      const double total_ms = (BLI_time_now_seconds() - start_time) * 1000.0;
      const SculptMeshV3MemorySample end_memory = sculpt_mesh_v3_memory_sample();

      const uint64_t page_fault_delta =
          (start_memory.valid && end_memory.valid &&
           end_memory.page_faults >= start_memory.page_faults) ?
              end_memory.page_faults - start_memory.page_faults :
              0;

      if (total_ms < 0.50 && page_fault_delta == 0) {
        return;
      }

      constexpr double mb = 1024.0 * 1024.0;
      const double ws_mb = end_memory.valid ? double(end_memory.working_set) / mb : 0.0;
      const double private_mb = end_memory.valid ? double(end_memory.private_bytes) / mb : 0.0;
      const double avail_mb = end_memory.avail_phys ? double(end_memory.avail_phys) / mb : 0.0;
      const double ws_delta_mb =
          (start_memory.valid && end_memory.valid) ?
              (double(int64_t(end_memory.working_set) -
                      int64_t(start_memory.working_set)) / mb) :
              0.0;
      const double private_delta_mb =
          (start_memory.valid && end_memory.valid) ?
              (double(int64_t(end_memory.private_bytes) -
                      int64_t(start_memory.private_bytes)) / mb) :
              0.0;

      std::fprintf(stderr,
                   "[SculptMeshV3Mem] brush=%s total=%.3f ms pf=+%llu "
                   "ws=%.1f MB dws=%+.2f MB private=%.1f MB dprivate=%+.2f MB "
                   "avail=%.1f MB\n",
                   sculpt_brush_type_name(brush),
                   total_ms,
                   static_cast<unsigned long long>(page_fault_delta),
                   ws_mb,
                   ws_delta_mb,
                   private_mb,
                   private_delta_mb,
                   avail_mb);
    }
  } sculpt_mesh_v3_memory_probe_scope{
      sculpt_mesh_v3_probe_enabled,
      brush,
      sculpt_mesh_v3_mem_start,
      BLI_time_now_seconds()};
  IndexMaskMemory memory;
  IndexMask texnode_mask;

  const bool use_original = brush_type_needs_original(brush.sculpt_brush_type) ? true :
                                                                                 !ss.cache->accum;
  const bool use_pixels = sculpt_needs_pbvh_pixels(brush, ob);

  if (sculpt_needs_pbvh_pixels(brush, ob)) {
    sculpt_pbvh_update_pixels(depsgraph, ob);

    texnode_mask = pbvh_gather_texpaint(ob, brush, use_original, 1.0f, memory);

    if (texnode_mask.is_empty()) {
      return;
    }
  }

  const double sculpt_mesh_v3_probe_gather_start = BLI_time_now_seconds();
  const brushes::CursorSampleResult cursor_sample_result = calc_brush_node_mask(
      depsgraph, ob, brush, memory);
  const IndexMask node_mask = cursor_sample_result.node_mask;
  sculpt_mesh_v3_probe_gather_ms =
      (BLI_time_now_seconds() - sculpt_mesh_v3_probe_gather_start) * 1000.0;
  sculpt_mesh_v3_probe_nodes = node_mask.size();

  /* Only act if some verts are inside the brush area. */
  if (node_mask.is_empty()) {
    return;
  }

  if (auto_mask::is_enabled(sd.paint, ob, &brush)) {
    auto_mask::Cache &cache = auto_mask::stroke_cache_ensure(depsgraph, sd.paint, &brush, ob);
    if (cache.settings.flags & BRUSH_AUTOMASKING_CAVITY_ALL) {
      cache.calc_cavity_factor(depsgraph, ob, node_mask);
    }
  }

  const double sculpt_mesh_v3_probe_after_gather = BLI_time_now_seconds();
  if (!use_pixels) {
    const double sculpt_mesh_v3_probe_undo_start = BLI_time_now_seconds();
    push_undo_nodes(depsgraph, ob, brush, node_mask);
    sculpt_mesh_v3_probe_undo_ms =
        (BLI_time_now_seconds() - sculpt_mesh_v3_probe_undo_start) * 1000.0;
  }

  /* Capture Auto Smooth's reference before the main brush changes the CCG. */
  if (!layer_delta_smooth_v01::prepare_region(ob, node_mask)) { return; }

  /* There are issues with the underlying normals cache / mesh data that can cause the data to
   * become out of date.
   *
   * For EEVEE and Workbench, this is partially mitigated by the fact that the Paint BVH is used
   * to signal this update when drawing.
   *
   * TODO: See #141417
   */
  const bool external_engine = ss.rv3d && ss.rv3d->view_render != nullptr;
  if (external_engine) {
    bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
    bke::pbvh::update_normals(depsgraph, ob, pbvh);
  }
  if (sculpt_brush_needs_normal(ss, brush)) {
    update_sculpt_normal(depsgraph, sd, ob, cursor_sample_result);
  }

  update_brush_local_mat(sd, ob);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try {
    anchor_surface_path_prepare(brush, ob);
  }
  catch (const std::bad_alloc &) {
    /* Allocation failure is a resource failure, never a fixed sample-count cap.
     * Release local caches and retain the existing V1/V2 projection for this dab. */
    ss.cache->anchor_surface_path.reset();
    ss.cache->anchor_surface_passes.clear();
    ss.cache->anchor_surface_allocation_failed = true;
    if (ss.cache->anchor_projection_diagnostics) {
      ss.cache->anchor_projection_diagnostics->reason = "allocation_failure_v1_v2";
    }
    std::printf("[ANCHOR_V5] Local cache allocation failed; using V1/V2 for this preview.\n");
  }
#else
  anchor_surface_path_prepare(brush, ob);
#endif

  if (brush.deform_target == BRUSH_DEFORM_TARGET_CLOTH_SIM) {
    if (!ss.cache->cloth_sim) {
      ss.cache->cloth_sim = cloth::brush_simulation_create(
          depsgraph, ob, 1.0f, 0.0f, 0.0f, false, true);
    }
    cloth::brush_store_simulation_state(depsgraph, ob, *ss.cache->cloth_sim);
    cloth::ensure_nodes_constraints(sd,
                                    ob,
                                    node_mask,
                                    *ss.cache->cloth_sim,
                                    ss.cache->location_symm,
                                    std::numeric_limits<float>::max());
  }

  sculpt_mesh_v3_probe_prebrush_ms =
      (BLI_time_now_seconds() - sculpt_mesh_v3_probe_after_gather) * 1000.0 -
      sculpt_mesh_v3_probe_undo_ms;
  /* Apply one type of brush action. */
  switch (brush.sculpt_brush_type) {
    case SCULPT_BRUSH_TYPE_DRAW: {
      if (brush_uses_vector_displacement(brush)) {
        brushes::do_draw_vector_displacement_brush(depsgraph, sd, ob, node_mask);
      }
      else {
        brushes::do_draw_brush(depsgraph, sd, ob, node_mask);
      }
      break;
    }
    case SCULPT_BRUSH_TYPE_SMOOTH:
      if (layer_delta_smooth_v01::owns(ob) &&
          (brush.smooth_deform_type != BRUSH_SMOOTH_DEFORM_LAPLACIAN ||
           ss.cache->initial_direction_flipped))
      {
        layer_delta_smooth_v01::fail("surface_or_inverted_smooth_not_supported_v01");
        break;
      }
      if (brush.smooth_deform_type == BRUSH_SMOOTH_DEFORM_LAPLACIAN) {
        /* NOTE: The enhance brush needs to initialize its state on the first brush step. The
         * stroke strength can become 0 during the stroke, but it can not change sign (the sign is
         * determined in the beginning of the stroke). So here it is important to not switch to
         * enhance brush in the middle of the stroke. */
        if (ss.cache->initial_direction_flipped) {
          /* Invert mode, intensify details. */
          brushes::do_enhance_details_brush(depsgraph, sd, ob, node_mask);
        }
        else {
          brushes::do_smooth_brush(
              depsgraph, sd, ob, node_mask, std::clamp(ss.cache->bstrength, 0.0f, 1.0f));
        }
      }
      else if (brush.smooth_deform_type == BRUSH_SMOOTH_DEFORM_SURFACE) {
        brushes::do_surface_smooth_brush(depsgraph, sd, ob, node_mask);
      }
      break;
    case SCULPT_BRUSH_TYPE_CREASE:
      brushes::do_crease_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_BLOB:
      brushes::do_blob_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_PINCH:
      brushes::do_pinch_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_INFLATE:
      brushes::do_inflate_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_GRAB:
      brushes::do_grab_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_ROTATE:
      brushes::do_rotate_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_SNAKE_HOOK:
      brushes::do_snake_hook_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_NUDGE:
      brushes::do_nudge_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_THUMB:
      brushes::do_thumb_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_LAYER:
      brushes::do_layer_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_CLAY:
      brushes::do_clay_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_CLAY_STRIPS:
      BLI_assert(cursor_sample_result.plane_normal && cursor_sample_result.plane_center);
      brushes::do_clay_strips_brush(depsgraph,
                                    sd,
                                    ob,
                                    node_mask,
                                    *cursor_sample_result.plane_normal,
                                    *cursor_sample_result.plane_center);
      break;
    case SCULPT_BRUSH_TYPE_MULTIPLANE_SCRAPE:
      brushes::do_multiplane_scrape_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_CLAY_THUMB:
      brushes::do_clay_thumb_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_MASK:
      switch (BrushMaskTool(brush.mask_tool)) {
        case BRUSH_MASK_DRAW:
          brushes::do_mask_brush(depsgraph, sd, ob, node_mask);
          break;
        case BRUSH_MASK_SMOOTH:
          brushes::do_smooth_mask_brush(depsgraph, sd, ob, node_mask, ss.cache->bstrength);
          break;
      }
      break;
    case SCULPT_BRUSH_TYPE_POSE:
      pose::do_pose_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_DRAW_SHARP:
      brushes::do_draw_sharp_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_ELASTIC_DEFORM:
      brushes::do_elastic_deform_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_SLIDE_RELAX:
      if (ss.cache->toggle_settings.alt_smooth) {
        brushes::do_topology_relax_brush(depsgraph, sd, ob, node_mask);
      }
      else {
        brushes::do_topology_slide_brush(depsgraph, sd, ob, node_mask);
      }
      break;
    case SCULPT_BRUSH_TYPE_BOUNDARY:
      boundary::do_boundary_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_CLOTH:
      cloth::do_cloth_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_DRAW_FACE_SETS:
      if (!ss.cache->toggle_settings.alt_smooth) {
        brushes::do_draw_face_sets_brush(depsgraph, sd, ob, node_mask);
      }
      else {
        brushes::do_relax_face_sets_brush(depsgraph, sd, ob, node_mask);
      }
      break;
    case SCULPT_BRUSH_TYPE_DISPLACEMENT_ERASER:
      brushes::do_displacement_eraser_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_DISPLACEMENT_SMEAR:
      brushes::do_displacement_smear_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_PAINT:
      color::do_paint_brush(depsgraph, paint_mode_settings, sd, ob, node_mask, texnode_mask);
      break;
    case SCULPT_BRUSH_TYPE_SMEAR:
      color::do_smear_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_PLANE:
      BLI_assert(cursor_sample_result.plane_normal && cursor_sample_result.plane_center);
      brushes::do_plane_brush(depsgraph,
                              sd,
                              ob,
                              node_mask,
                              *cursor_sample_result.plane_normal,
                              *cursor_sample_result.plane_center);
      break;
    case SCULPT_BRUSH_TYPE_BLUR:
      color::do_blur_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_SCENE_PROJECT:
      brushes::do_scene_project_brush(depsgraph, sd, ob, node_mask);
      break;
    case SCULPT_BRUSH_TYPE_SIMPLIFY:
      break;
  }

  /* Face Set painting changes IDs only. Do not apply Auto Smooth to geometry
   * while recording Face Sets independently of the selected sculpt layer. */
  if (!ELEM(brush.sculpt_brush_type,
            SCULPT_BRUSH_TYPE_SMOOTH,
            SCULPT_BRUSH_TYPE_MASK,
            SCULPT_BRUSH_TYPE_DRAW_FACE_SETS) &&
      brush.autosmooth_factor > 0)
  {
    if (bke::brush::supports_auto_smooth_pressure(brush) &&
        brush.flag & BRUSH_INVERSE_SMOOTH_PRESSURE)
    {
      brushes::do_smooth_brush(
          depsgraph, sd, ob, node_mask, brush.autosmooth_factor * (1.0f - ss.cache->pressure));
    }
    else {
      brushes::do_smooth_brush(depsgraph, sd, ob, node_mask, brush.autosmooth_factor);
    }
  }

  if (brush_uses_topology_rake(ss, brush)) {
    brushes::do_bmesh_topology_rake_brush(
        depsgraph, sd, ob, node_mask, brush.topology_rake_factor);
  }

  /* The cloth brush adds the gravity as a regular force and it is processed in the solver. */
  if (ss.cache->supports_gravity && brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_CLOTH) {
    brushes::do_gravity_brush(depsgraph, sd, ob, node_mask);
  }

  if (brush.deform_target == BRUSH_DEFORM_TARGET_CLOTH_SIM) {
    if (stroke_is_main_symmetry_pass(*ss.cache)) {
      cloth::sim_activate_nodes(ob, *ss.cache->cloth_sim, node_mask);
      cloth::do_simulation_step(depsgraph, sd, ob, *ss.cache->cloth_sim, node_mask);
    }
  }

  anchor_diagnostic_report(*ss.cache);

  /* Update average stroke position. */
  const float3 world_location = math::project_point(ob.object_to_world(), ss.cache->location);

  bke::PaintRuntime &paint_runtime = *sd.paint.runtime;
  add_v3_v3(paint_runtime.average_stroke_accum, world_location);
  paint_runtime.average_stroke_counter++;
  /* Update last stroke position. */
  paint_runtime.last_stroke_valid = true;
}

void cache_calc_brushdata_symm(StrokeCache &cache,
                               const ePaintSymmetryFlags symm,
                               const char axis,
                               const float angle)
{
  cache.location_symm = symmetry_flip(cache.location, symm);
  cache.last_location_symm = symmetry_flip(cache.last_location, symm);
  cache.grab_delta_symm = symmetry_flip(cache.grab_delta, symm);
  cache.view_normal_symm = symmetry_flip(cache.view_normal, symm);
  cache.view_origin_symm = symmetry_flip(cache.view_origin, symm);

  cache.initial_location_symm = symmetry_flip(cache.initial_location, symm);
  cache.initial_normal_symm = symmetry_flip(cache.initial_normal, symm);

  /* XXX This reduces the length of the grab delta if it approaches the line of symmetry
   * XXX However, a different approach appears to be needed. */
#if 0
  if (sd->paint.symmetry_flags & PAINT_SYMMETRY_FEATHER) {
    float frac = 1.0f / max_overlap_count(sd);
    float reduce = (feather - frac) / (1.0f - frac);

    printf("feather: %f frac: %f reduce: %f\n", feather, frac, reduce);

    if (frac < 1.0f) {
      mul_v3_fl(cache.grab_delta_symmetry, reduce);
    }
  }
#endif

  cache.symm_rot_mat = float4x4::identity();
  cache.symm_rot_mat_inv = float4x4::identity();
  zero_v3(cache.plane_offset);

  /* Expects XYZ. */
  if (axis) {
    rotate_m4(cache.symm_rot_mat.ptr(), axis, angle);
    rotate_m4(cache.symm_rot_mat_inv.ptr(), axis, -angle);
  }

  mul_m4_v3(cache.symm_rot_mat.ptr(), cache.location_symm);
  mul_m4_v3(cache.symm_rot_mat.ptr(), cache.grab_delta_symm);

  if (cache.supports_gravity) {
    cache.gravity_direction_symm = symmetry_flip(cache.gravity_direction, symm);
    mul_m4_v3(cache.symm_rot_mat.ptr(), cache.gravity_direction_symm);
  }

  if (cache.rake_rotation) {
    float4 new_quat;
    float4 existing(cache.rake_rotation->w,
                    cache.rake_rotation->x,
                    cache.rake_rotation->y,
                    cache.rake_rotation->z);
    flip_qt_qt(new_quat, existing, symm);
    cache.rake_rotation_symm = math::Quaternion(new_quat);
  }
}

using BrushActionFunc = void (*)(const Depsgraph &depsgraph,
                                 const Scene &scene,
                                 Sculpt &sd,
                                 Object &ob,
                                 const Brush &brush,
                                 PaintModeSettings &paint_mode_settings);

static void do_tiled(const Depsgraph &depsgraph,
                     const Scene &scene,
                     Sculpt &sd,
                     Object &ob,
                     const Brush &brush,
                     PaintModeSettings &paint_mode_settings,
                     const BrushActionFunc action)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache *cache = ss.cache;
  const float radius = cache->radius;
  const Bounds<float3> bb = *BKE_object_boundbox_get(&ob);
  const float *bbMin = bb.min;
  const float *bbMax = bb.max;
  const float *step = sd.paint.tile_offset;

  /* These are integer locations, for real location: multiply with step and add orgLoc.
   * So 0,0,0 is at orgLoc. */
  int start[3];
  int end[3];
  int cur[3];

  /* Position of the "prototype" stroke for tiling. */
  float orgLoc[3];
  float original_initial_location[3];
  copy_v3_v3(orgLoc, cache->location_symm);
  copy_v3_v3(original_initial_location, cache->initial_location_symm);

  for (int dim = 0; dim < 3; dim++) {
    if ((sd.paint.symmetry_flags & (PAINT_TILE_X << dim)) && step[dim] > 0) {
      start[dim] = (bbMin[dim] - orgLoc[dim] - radius) / step[dim];
      end[dim] = (bbMax[dim] - orgLoc[dim] + radius) / step[dim];
    }
    else {
      start[dim] = end[dim] = 0;
    }
  }

  /* First do the "un-tiled" position to initialize the stroke for this location. */
  cache->tile_pass = 0;
  action(depsgraph, scene, sd, ob, brush, paint_mode_settings);

  /* Now do it for all the tiles. */
  copy_v3_v3_int(cur, start);
  for (cur[0] = start[0]; cur[0] <= end[0]; cur[0]++) {
    for (cur[1] = start[1]; cur[1] <= end[1]; cur[1]++) {
      for (cur[2] = start[2]; cur[2] <= end[2]; cur[2]++) {
        if (!cur[0] && !cur[1] && !cur[2]) {
          /* Skip tile at orgLoc, this was already handled before all others. */
          continue;
        }

        ++cache->tile_pass;

        for (int dim = 0; dim < 3; dim++) {
          cache->location_symm[dim] = cur[dim] * step[dim] + orgLoc[dim];
          cache->plane_offset[dim] = cur[dim] * step[dim];
          cache->initial_location_symm[dim] = cur[dim] * step[dim] +
                                              original_initial_location[dim];
        }
        action(depsgraph, scene, sd, ob, brush, paint_mode_settings);
      }
    }
  }
}

static void do_radial_symmetry(const Depsgraph &depsgraph,
                               const Scene &scene,
                               Sculpt &sd,
                               Object &ob,
                               const Brush &brush,
                               PaintModeSettings &paint_mode_settings,
                               const BrushActionFunc action,
                               const ePaintSymmetryFlags symm,
                               const int axis,
                               const float /*feather*/)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const Mesh &mesh = *id_cast<Mesh *>(ob.data);

  for (int i = 1; i < mesh.radial_symmetry[axis - 'X']; i++) {
    const float angle = 2.0f * M_PI * i / mesh.radial_symmetry[axis - 'X'];
    ss.cache->radial_symmetry_pass = i;
    cache_calc_brushdata_symm(*ss.cache, symm, axis, angle);
    do_tiled(depsgraph, scene, sd, ob, brush, paint_mode_settings, action);
  }
}

/**
 * Noise texture gives different values for the same input coord; this
 * can tear a multi-resolution mesh during sculpting so do a stitch in this case.
 */
static void sculpt_fix_noise_tear(const Sculpt &sd, Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);

  if (ss.multires_modifier && mtex->tex && mtex->tex->type == TEX_NOISE) {
    multires_stitch_grids(&ob);
  }
}

static void do_symmetrical_brush_actions(const Depsgraph &depsgraph,
                                         const Scene &scene,
                                         Sculpt &sd,
                                         Object &ob,
                                         const BrushActionFunc action,
                                         PaintModeSettings &paint_mode_settings)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const Mesh &mesh = *id_cast<Mesh *>(ob.data);
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(ob);

  float feather = calc_symmetry_feather(sd, symm, mesh, *ss.cache);

  cache.bstrength = brush_strength(sd, cache, feather, paint_mode_settings);
  if (sculpt_uniform_accumulation_enabled(brush)) {
    cache.bstrength *= sd.paint.runtime->overlap_factor;
  }

  /* `symm` is a bit combination of XYZ -
   * 1 is mirror X; 2 is Y; 3 is XY; 4 is Z; 5 is XZ; 6 is YZ; 7 is XYZ */
  for (int i = 0; i <= symm; i++) {
    if (!is_symmetry_iteration_valid(i, symm)) {
      continue;
    }
    const ePaintSymmetryFlags symm = ePaintSymmetryFlags(i);
    cache.mirror_symmetry_pass = symm;
    cache.radial_symmetry_pass = 0;

    cache_calc_brushdata_symm(cache, symm, 0, 0);
    do_tiled(depsgraph, scene, sd, ob, brush, paint_mode_settings, action);

    do_radial_symmetry(
        depsgraph, scene, sd, ob, brush, paint_mode_settings, action, symm, 'X', feather);
    do_radial_symmetry(
        depsgraph, scene, sd, ob, brush, paint_mode_settings, action, symm, 'Y', feather);
    do_radial_symmetry(
        depsgraph, scene, sd, ob, brush, paint_mode_settings, action, symm, 'Z', feather);
  }
}

bool sculpt_mode_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  return ob && ob->mode & OB_MODE_SCULPT;
}

bool sculpt_mode_poll_view3d(bContext *C)
{
  return (sculpt_mode_poll(C) && CTX_wm_region_view3d(C));
}

bool sculpt_mode_and_brush_poll(bContext *C)
{
  return sculpt_mode_poll(C) && paint_brush_tool_poll(C);
}

/**
 * While most non-brush tools in sculpt mode do not use the brush cursor, the trim tools
 * and the filter tools are expected to have the cursor visible so that some functionality is
 * easier to visually estimate.
 *
 * See: #122856
 */
static bool is_brush_related_tool(bContext *C)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Object *ob = CTX_data_active_object(C);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);

  if (paint && ob && BKE_paint_brush(paint) &&
      (area && ELEM(area->spacetype, SPACE_VIEW3D, SPACE_IMAGE)) &&
      (region && region->regiontype == RGN_TYPE_WINDOW))
  {
    bToolRef *tref = area->runtime.tool;
    if (tref && tref->runtime && tref->runtime->keymap[0]) {
      std::array<wmOperatorType *, 7> trim_operators = {
          WM_operatortype_find("SCULPT_OT_trim_box_gesture", false),
          WM_operatortype_find("SCULPT_OT_trim_lasso_gesture", false),
          WM_operatortype_find("SCULPT_OT_trim_line_gesture", false),
          WM_operatortype_find("SCULPT_OT_trim_polyline_gesture", false),
          WM_operatortype_find("SCULPT_OT_mesh_filter", false),
          WM_operatortype_find("SCULPT_OT_cloth_filter", false),
          WM_operatortype_find("SCULPT_OT_color_filter", false),
      };

      return std::any_of(trim_operators.begin(), trim_operators.end(), [tref](wmOperatorType *ot) {
        PointerRNA ptr;
        return WM_toolsystem_ref_properties_get_from_operator(tref, ot, &ptr);
      });
    }
  }
  return false;
}

bool brush_cursor_poll(bContext *C)
{
  return sculpt_mode_poll(C) && (paint_brush_cursor_poll(C) || is_brush_related_tool(C));
}

StrokeCache::StrokeCache() = default;

StrokeCache::~StrokeCache()
{
  if (this->dial) {
    BLI_dial_free(this->dial);
  }
}

}  // namespace ed::sculpt_paint

enum class StrokeFlags : uint8_t {
  ClipX = 1,
  ClipY = 2,
  ClipZ = 4,
};

namespace ed::sculpt_paint {

/* Initialize mirror modifier clipping. */
static void sculpt_init_mirror_clipping(const Object &ob, const SculptSession &ss)
{
  ss.cache->mirror_modifier_clip.mat = float4x4::identity();

  for (ModifierData &md : ob.modifiers) {
    if (!(md.type == eModifierType_Mirror && (md.mode & eModifierMode_Realtime))) {
      continue;
    }
    MirrorModifierData *mmd = reinterpret_cast<MirrorModifierData *>(&md);

    if (!(mmd->flag & MOD_MIR_CLIPPING)) {
      continue;
    }
    /* Check each axis for mirroring. */
    for (int i = 0; i < 3; i++) {
      if (!(mmd->flag & (MOD_MIR_AXIS_X << i))) {
        continue;
      }
      /* Enable sculpt clipping. */
      ss.cache->mirror_modifier_clip.flag |= uint8_t(StrokeFlags::ClipX) << i;

      /* Update the clip tolerance. */
      ss.cache->mirror_modifier_clip.tolerance[i] = std::max(
          mmd->tolerance, ss.cache->mirror_modifier_clip.tolerance[i]);

      /* Store matrix for mirror object clipping. */
      if (mmd->mirror_ob) {
        const float4x4 mirror_ob_inv = math::invert(mmd->mirror_ob->object_to_world());
        mul_m4_m4m4(ss.cache->mirror_modifier_clip.mat.ptr(),
                    mirror_ob_inv.ptr(),
                    ob.object_to_world().ptr());
      }
    }
  }
  ss.cache->mirror_modifier_clip.mat_inv = math::invert(ss.cache->mirror_modifier_clip.mat);
}

static void smooth_brush_toggle_on(Main *bmain,
                                   Paint *paint,
                                   StrokeToggleSettings &toggle_settings)
{
  Brush *cur_brush = BKE_paint_brush(paint);

  if (cur_brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
    toggle_settings.original_brush_mask_tool = BrushMaskTool(cur_brush->mask_tool);
    cur_brush->mask_tool = BRUSH_MASK_SMOOTH;
    return;
  }

  if (ELEM(cur_brush->sculpt_brush_type,
           SCULPT_BRUSH_TYPE_SLIDE_RELAX,
           SCULPT_BRUSH_TYPE_DRAW_FACE_SETS))
  {
    /* Do nothing, this brush has its own smooth mode. */
    return;
  }

  /* Switch to the smooth brush if possible. */
  const char *target_asset = brush_type_is_paint(cur_brush->sculpt_brush_type) ? "Blur" : "Smooth";
  if (!BKE_paint_brush_set_essentials(bmain, paint, target_asset)) {
    BKE_paint_brush_set(paint, cur_brush);
    CLOG_WARN(&LOG, "Unable to switch to the '%s' essentials brush asset", target_asset);
    toggle_settings.original_active_brush = nullptr;
    return;
  }

  Brush *smooth_brush = BKE_paint_brush(paint);
  int cur_brush_size = BKE_brush_size_get(paint, cur_brush);

  toggle_settings.original_active_brush = cur_brush;

  toggle_settings.original_brush_size = BKE_brush_size_get(paint, smooth_brush);
  BKE_brush_size_set(paint, smooth_brush, cur_brush_size);
  bke::brush::common_pressure_curves_init(*smooth_brush);
}

static void smooth_brush_toggle_off(Paint *paint, StrokeCache *cache)
{
  Brush &brush = *BKE_paint_brush(paint);

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
    brush.mask_tool = cache->toggle_settings.original_brush_mask_tool;
    return;
  }

  if (ELEM(brush.sculpt_brush_type,
           SCULPT_BRUSH_TYPE_SLIDE_RELAX,
           SCULPT_BRUSH_TYPE_DRAW_FACE_SETS))
  {
    /* Do nothing. */
    return;
  }

  /* If saved_active_brush is not set, brush was not switched/affected in
   * smooth_brush_toggle_on(). */
  if (cache->toggle_settings.original_active_brush) {
    BKE_brush_size_set(paint, &brush, cache->toggle_settings.original_brush_size);
    BKE_paint_brush_set(paint, cache->toggle_settings.original_active_brush);
    cache->toggle_settings.original_active_brush = nullptr;
  }
}

static void mask_brush_toggle_on(Main *bmain, Paint *paint, StrokeToggleSettings &toggle_settings)
{
  Brush *cur_brush = BKE_paint_brush(paint);

  /* User is already using Mask brush */
  if (cur_brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
    toggle_settings.original_brush_mask_tool = BrushMaskTool(cur_brush->mask_tool);
    toggle_settings.original_active_brush = nullptr;
    return;
  }

  /* Save current brush */
  toggle_settings.original_active_brush = cur_brush;

  /* Switch to Mask essentials brush */
  if (!BKE_paint_brush_set_essentials(bmain, paint, "Mask")) {
    BKE_paint_brush_set(paint, cur_brush);
    toggle_settings.original_active_brush = nullptr;
    CLOG_WARN(&LOG, "Unable to switch to the 'Mask' essentials brush asset");
    return;
  }

  Brush *mask_brush = BKE_paint_brush(paint);

  /* Match brush size */
  const int cur_brush_size = BKE_brush_size_get(paint, cur_brush);
  toggle_settings.original_brush_size = BKE_brush_size_get(paint, mask_brush);
  BKE_brush_size_set(paint, mask_brush, cur_brush_size);

  if (mask_brush->curve_distance_falloff) {
    BKE_curvemapping_init(mask_brush->curve_distance_falloff);
  }

  if (mask_brush->curve_strength) {
    BKE_curvemapping_init(mask_brush->curve_strength);
  }
}

static void mask_brush_toggle_off(Paint *paint, StrokeCache *cache)
{
  Brush &brush = *BKE_paint_brush(paint);

  /* User was already using mask brush */
  if (cache->toggle_settings.original_active_brush == nullptr) {
    if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
      brush.mask_tool = cache->toggle_settings.original_brush_mask_tool;
    }
    return;
  }

  /* Restore previous brush */
  BKE_brush_size_set(paint, &brush, cache->toggle_settings.original_brush_size);
  BKE_paint_brush_set(paint, cache->toggle_settings.original_active_brush);
  cache->toggle_settings.original_active_brush = nullptr;
}

static void init_scene_project_brush_targets(const Depsgraph &depsgraph,
                                             ViewLayer &view_layer,
                                             const View3D &v3d,
                                             const Object &active_object,
                                             StrokeCache &cache)
{
  cache.project_targets.clear();

  for (Base &base : *BKE_view_layer_object_bases_get(&view_layer)) {
    const bool is_active_object = base.object == &active_object;
    const bool is_hidden = !BKE_base_is_visible(&v3d, &base);
    Object *object = DEG_get_evaluated(&depsgraph, base.object);

    if (is_active_object || object->type != OB_MESH || is_hidden) {
      continue;
    }

    const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(object);
    if (!mesh_eval) {
      continue;
    }

    bke::BVHTreeFromMesh tree_data = mesh_eval->bvh_corner_tris();

    if (tree_data.tree == nullptr) {
      continue;
    }

    const float4x4 active_to_target_matrix = object->world_to_object() *
                                             active_object.object_to_world();

    ProjectBrushTarget project_target{std::move(tree_data), active_to_target_matrix};
    cache.project_targets.append(std::move(project_target));
  }
}

static float brush_dynamic_size_get(const Brush &brush,
                                    const StrokeCache &cache,
                                    float initial_size)
{
  const float pressure_eval = BKE_curvemapping_evaluateF(brush.curve_size, 0, cache.pressure);
  switch (brush.sculpt_brush_type) {
    case SCULPT_BRUSH_TYPE_CLAY:
      return max_ff(initial_size * 0.20f, initial_size * pow3f(pressure_eval));
    case SCULPT_BRUSH_TYPE_CLAY_STRIPS:
      return max_ff(initial_size * 0.30f, initial_size * powf(pressure_eval, 1.5f));
    case SCULPT_BRUSH_TYPE_CLAY_THUMB: {
      float clay_stabilized_pressure = brushes::clay_thumb_get_stabilized_pressure(cache);
      return initial_size *
             BKE_curvemapping_evaluateF(brush.curve_size, 0, clay_stabilized_pressure);
    }
    default:
      return initial_size * pressure_eval;
  }
}

/* In these brushes the grab delta is calculated always from the initial stroke location, which is
 * generally used to create grab deformations. */
static bool need_delta_from_anchored_origin(const Brush &brush)
{
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SMEAR &&
      (brush.stroke_method == BRUSH_STROKE_ANCHORED))
  {
    return true;
  }

  if (ELEM(brush.sculpt_brush_type,
           SCULPT_BRUSH_TYPE_GRAB,
           SCULPT_BRUSH_TYPE_POSE,
           SCULPT_BRUSH_TYPE_BOUNDARY,
           SCULPT_BRUSH_TYPE_THUMB,
           SCULPT_BRUSH_TYPE_ELASTIC_DEFORM))
  {
    return true;
  }
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH &&
      brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_GRAB)
  {
    return true;
  }
  return false;
}

/* In these brushes the grab delta is calculated from the previous stroke location, which is used
 * to calculate to orientate the brush tip and deformation towards the stroke direction. */
static bool need_delta_for_tip_orientation(const Brush &brush)
{
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH) {
    return brush.cloth_deform_type != BRUSH_CLOTH_DEFORM_GRAB;
  }
  return ELEM(brush.sculpt_brush_type,
              SCULPT_BRUSH_TYPE_CLAY_STRIPS,
              SCULPT_BRUSH_TYPE_PINCH,
              SCULPT_BRUSH_TYPE_MULTIPLANE_SCRAPE,
              SCULPT_BRUSH_TYPE_CLAY_THUMB,
              SCULPT_BRUSH_TYPE_NUDGE,
              SCULPT_BRUSH_TYPE_SNAKE_HOOK);
}

static void brush_delta_update(const Depsgraph &depsgraph,
                               Paint &paint,
                               const Object &ob,
                               const Brush &brush)
{
  bke::PaintRuntime &paint_runtime = *paint.runtime;
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  StrokeCache *cache = ss.cache;
  const float mval[2] = {
      cache->mouse_event[0],
      cache->mouse_event[1],
  };
  int brush_type = brush.sculpt_brush_type;

  if (!ELEM(brush_type,
            SCULPT_BRUSH_TYPE_PAINT,
            SCULPT_BRUSH_TYPE_GRAB,
            SCULPT_BRUSH_TYPE_ELASTIC_DEFORM,
            SCULPT_BRUSH_TYPE_CLOTH,
            SCULPT_BRUSH_TYPE_NUDGE,
            SCULPT_BRUSH_TYPE_CLAY_STRIPS,
            SCULPT_BRUSH_TYPE_PLANE,
            SCULPT_BRUSH_TYPE_PINCH,
            SCULPT_BRUSH_TYPE_MULTIPLANE_SCRAPE,
            SCULPT_BRUSH_TYPE_CLAY_THUMB,
            SCULPT_BRUSH_TYPE_SNAKE_HOOK,
            SCULPT_BRUSH_TYPE_POSE,
            SCULPT_BRUSH_TYPE_BOUNDARY,
            SCULPT_BRUSH_TYPE_SMEAR,
            SCULPT_BRUSH_TYPE_THUMB) &&
      !brush_uses_topology_rake(ss, brush))
  {
    return;
  }
  float grab_location[3], imat[4][4], delta[3], loc[3];

  if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    if (brush_type == SCULPT_BRUSH_TYPE_GRAB && brush.flag & BRUSH_GRAB_ACTIVE_VERTEX &&
        !std::holds_alternative<std::monostate>(ss.active_vert()))
    {
      if (pbvh.type() == bke::pbvh::Type::Mesh) {
        const Span<float3> positions = vert_positions_for_grab_active_get(depsgraph, ob);
        cache->orig_grab_location = positions[std::get<int>(ss.active_vert())];
      }
      else {
        cache->orig_grab_location = ss.active_vert_position(depsgraph, ob);
      }
    }
    else {
      copy_v3_v3(cache->orig_grab_location, cache->location);
    }
  }
  else if (brush_type == SCULPT_BRUSH_TYPE_SNAKE_HOOK ||
           (brush_type == SCULPT_BRUSH_TYPE_CLOTH &&
            brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_SNAKE_HOOK))
  {
    add_v3_v3(cache->location, cache->grab_delta);
  }

  /* Compute 3d coordinate at same z from original location + mval. */
  mul_v3_m4v3(loc, ob.object_to_world().ptr(), cache->orig_grab_location);
  ED_view3d_win_to_3d(cache->vc->v3d, cache->vc->region, loc, mval, grab_location);

  /* Compute delta to move verts by. */
  if (!stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    if (need_delta_from_anchored_origin(brush)) {
      sub_v3_v3v3(delta, grab_location, cache->old_grab_location);
      invert_m4_m4(imat, ob.object_to_world().ptr());
      mul_mat3_m4_v3(imat, delta);
      add_v3_v3(cache->grab_delta, delta);
    }
    else if (need_delta_for_tip_orientation(brush)) {
      if (brush.stroke_method == BRUSH_STROKE_ANCHORED) {
        float orig[3];
        mul_v3_m4v3(orig, ob.object_to_world().ptr(), cache->orig_grab_location);
        sub_v3_v3v3(cache->grab_delta, grab_location, orig);
      }
      else {
        sub_v3_v3v3(cache->grab_delta, grab_location, cache->old_grab_location);
      }
      invert_m4_m4(imat, ob.object_to_world().ptr());
      mul_mat3_m4_v3(imat, cache->grab_delta);
    }
    else {
      /* Use for 'Brush.topology_rake_factor'. */
      sub_v3_v3v3(cache->grab_delta, grab_location, cache->old_grab_location);
    }
  }
  else {
    zero_v3(cache->grab_delta);
  }

  if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
    project_plane_v3_v3v3(cache->grab_delta, cache->grab_delta, ss.cache->view_normal);
  }

  copy_v3_v3(cache->old_grab_location, grab_location);

  if (need_delta_from_anchored_origin(brush)) {
    /* Location stays the same for finding vertices in brush radius. */
    copy_v3_v3(cache->location, cache->orig_grab_location);

    paint_runtime.draw_anchored = true;
    copy_v2_v2(paint_runtime.anchored_initial_mouse, cache->initial_mouse);
    paint_runtime.anchored_size = paint_runtime.pixel_radius;
  }

  /* Handle 'rake' */
  cache->rake_rotation = std::nullopt;
  cache->rake_rotation_symm = std::nullopt;
  invert_m4_m4(imat, ob.object_to_world().ptr());
  mul_mat3_m4_v3(imat, grab_location);

  if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    copy_v3_v3(cache->rake_data.follow_co, grab_location);
  }

  if (!brush_needs_rake_rotation(brush)) {
    return;
  }
  cache->rake_data.follow_dist = cache->radius * SCULPT_RAKE_BRUSH_FACTOR;

  if (!is_zero_v3(cache->grab_delta)) {
    const float eps = 0.00001f;

    float v1[3], v2[3];

    copy_v3_v3(v1, cache->rake_data.follow_co);
    copy_v3_v3(v2, cache->rake_data.follow_co);
    sub_v3_v3(v2, cache->grab_delta);

    sub_v3_v3(v1, grab_location);
    sub_v3_v3(v2, grab_location);

    if ((normalize_v3(v2) > eps) && (normalize_v3(v1) > eps) && (len_squared_v3v3(v1, v2) > eps)) {
      const float rake_dist_sq = len_squared_v3v3(cache->rake_data.follow_co, grab_location);
      const float rake_fade = (rake_dist_sq > square_f(cache->rake_data.follow_dist)) ?
                                  1.0f :
                                  sqrtf(rake_dist_sq) / cache->rake_data.follow_dist;

      const math::AxisAngle between_vecs(v1, v2);
      const math::AxisAngle rotated(between_vecs.axis(),
                                    between_vecs.angle() * brush.rake_factor * rake_fade);
      cache->rake_rotation = math::to_quaternion(rotated);
    }
  }
  rake_data_update(&cache->rake_data, grab_location);
}

static void cache_paint_invariants_update(StrokeCache &cache, const Brush &brush)
{
  cache.hardness = brush.hardness;
  if (bke::brush::supports_hardness_pressure(brush) &&
      brush.paint_flags & BRUSH_PAINT_HARDNESS_PRESSURE)
  {
    cache.hardness *= brush.paint_flags & BRUSH_PAINT_HARDNESS_PRESSURE_INVERT ?
                          1.0f - cache.pressure :
                          cache.pressure;
  }

  cache.paint_brush.flow = brush.flow;
  if (brush.paint_flags & BRUSH_PAINT_FLOW_PRESSURE) {
    cache.paint_brush.flow *= brush.paint_flags & BRUSH_PAINT_FLOW_PRESSURE_INVERT ?
                                  1.0f - cache.pressure :
                                  cache.pressure;
  }

  cache.paint_brush.wet_mix = brush.wet_mix;
  if (brush.paint_flags & BRUSH_PAINT_WET_MIX_PRESSURE) {
    cache.paint_brush.wet_mix *= brush.paint_flags & BRUSH_PAINT_WET_MIX_PRESSURE_INVERT ?
                                     1.0f - cache.pressure :
                                     cache.pressure;

    /* This makes wet mix more sensible in higher values, which allows to create brushes that have
     * a wider pressure range were they only blend colors without applying too much of the brush
     * color. */
    cache.paint_brush.wet_mix = 1.0f - pow2f(1.0f - cache.paint_brush.wet_mix);
  }

  cache.paint_brush.wet_persistence = brush.wet_persistence;
  if (brush.paint_flags & BRUSH_PAINT_WET_PERSISTENCE_PRESSURE) {
    cache.paint_brush.wet_persistence = brush.paint_flags &
                                                BRUSH_PAINT_WET_PERSISTENCE_PRESSURE_INVERT ?
                                            1.0f - cache.pressure :
                                            cache.pressure;
  }

  cache.paint_brush.density = brush.density;
  if (brush.paint_flags & BRUSH_PAINT_DENSITY_PRESSURE) {
    cache.paint_brush.density = brush.paint_flags & BRUSH_PAINT_DENSITY_PRESSURE_INVERT ?
                                    1.0f - cache.pressure :
                                    cache.pressure;
  }
}

/* Returns true if any of the smoothing modes are active (currently
 * one of smooth brush, autosmooth, mask smooth, or shift-key
 * smooth). */
static bool sculpt_needs_connectivity_info(const Sculpt &sd,
                                           const Brush &brush,
                                           const Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh && auto_mask::is_enabled(sd.paint, object, &brush)) {
    return true;
  }
  return ((ss.cache && ss.cache->toggle_settings.alt_smooth) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SMOOTH) || (brush.autosmooth_factor > 0) ||
          ((brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) &&
           (brush.mask_tool == BRUSH_MASK_SMOOTH)) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_POSE) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_BOUNDARY) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SLIDE_RELAX) ||
          brush_type_is_paint(brush.sculpt_brush_type) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SMEAR) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW_FACE_SETS) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_DISPLACEMENT_SMEAR) ||
          (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT));
}

void stroke_modifiers_check(
    Depsgraph &depsgraph, RegionView3D *rv3d, const Sculpt &sd, Object &ob, const Brush *brush)
{
  SculptSession &ss = *ob.runtime->sculpt_session;

  bool need_pmap = brush && sculpt_needs_connectivity_info(sd, *brush, ob);
  if (ss.shapekey_active || ss.deform_modifiers_active ||
      (!BKE_sculptsession_use_pbvh_draw(&ob, rv3d) && need_pmap))
  {
    BLI_assert(ss.pbvh->type() == bke::pbvh::Type::Mesh);
    BKE_sculpt_update_object_for_edit(
        &depsgraph, &ob, brush_type_is_paint(brush->sculpt_brush_type));
  }
}

void stroke_modifiers_check(const bContext *C, Object &ob, const Brush *brush)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;

  stroke_modifiers_check(*depsgraph, rv3d, sd, ob, brush);
}

static void sculpt_raycast_cb(bke::pbvh::Node &node, RaycastData &rd, float *tmin)
{
  if (BKE_pbvh_node_get_tmin(&node) >= *tmin) {
    return;
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*rd.object);
  bool use_origco = false;
  Span<float3> origco;
  if (rd.use_original && rd.is_mid_stroke) {
    switch (pbvh.type()) {
      case bke::pbvh::Type::Mesh:
        if (const std::optional<OrigPositionData> orig_data =
                orig_position_data_lookup_mesh_all_verts(
                    *rd.object, static_cast<const bke::pbvh::MeshNode &>(node)))
        {
          use_origco = true;
          origco = orig_data->positions;
        }
        break;
      case bke::pbvh::Type::Grids:
        if (const std::optional<OrigPositionData> orig_data = orig_position_data_lookup_grids(
                *rd.object, static_cast<const bke::pbvh::GridsNode &>(node)))
        {
          use_origco = true;
          origco = orig_data->positions;
        }
        break;
      case bke::pbvh::Type::BMesh:
        use_origco = true;
        break;
    }
  }

  if (node.flag_ & bke::pbvh::Node::FullyHidden) {
    return;
  }

  bool hit = false;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      int mesh_active_vert;
      hit = bke::pbvh::node_raycast_mesh(static_cast<bke::pbvh::MeshNode &>(node),
                                         origco,
                                         rd.vert_positions,
                                         rd.faces,
                                         rd.corner_verts,
                                         rd.corner_tris,
                                         rd.hide_poly,
                                         rd.ray_start,
                                         rd.ray_normal,
                                         &rd.isect_precalc,
                                         &rd.depth,
                                         mesh_active_vert,
                                         rd.active_face_grid_index,
                                         rd.face_normal);
      if (hit) {
        rd.active_vertex = mesh_active_vert;
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCGCoord grids_active_vert;
      hit = bke::pbvh::node_raycast_grids(*rd.subdiv_ccg,
                                          static_cast<bke::pbvh::GridsNode &>(node),
                                          origco,
                                          rd.ray_start,
                                          rd.ray_normal,
                                          &rd.isect_precalc,
                                          &rd.depth,
                                          grids_active_vert,
                                          rd.active_face_grid_index,
                                          rd.face_normal);
      if (hit) {
        rd.active_vertex = grids_active_vert.to_index(
            BKE_subdiv_ccg_key_top_level(*rd.subdiv_ccg));
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMVert *bmesh_active_vert;
      hit = bke::pbvh::node_raycast_bmesh(static_cast<bke::pbvh::BMeshNode &>(node),
                                          rd.ray_start,
                                          rd.ray_normal,
                                          &rd.isect_precalc,
                                          &rd.depth,
                                          use_origco,
                                          &bmesh_active_vert,
                                          rd.face_normal);
      if (hit) {
        rd.active_vertex = bmesh_active_vert;
      }
      break;
    }
  }

  if (hit) {
    rd.hit = true;
    *tmin = rd.depth;
  }
}

static void sculpt_find_nearest_to_ray_cb(bke::pbvh::Node &node,
                                          FindNearestToRayData &fntrd,
                                          float *tmin)
{
  if (BKE_pbvh_node_get_tmin(&node) >= *tmin) {
    return;
  }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*fntrd.object);
  bool use_origco = false;
  Span<float3> origco;
  if (fntrd.use_original && fntrd.is_mid_stroke) {
    switch (pbvh.type()) {
      case bke::pbvh::Type::Mesh:
        if (const std::optional<OrigPositionData> orig_data =
                orig_position_data_lookup_mesh_all_verts(
                    *fntrd.object, static_cast<const bke::pbvh::MeshNode &>(node)))
        {
          use_origco = true;
          origco = orig_data->positions;
        }
        break;
      case bke::pbvh::Type::Grids:
        if (const std::optional<OrigPositionData> orig_data = orig_position_data_lookup_grids(
                *fntrd.object, static_cast<const bke::pbvh::GridsNode &>(node)))
        {
          use_origco = true;
          origco = orig_data->positions;
        }

        break;
      case bke::pbvh::Type::BMesh:
        use_origco = true;
        break;
    }
  }

  if (bke::pbvh::find_nearest_to_ray_node(pbvh,
                                          node,
                                          origco,
                                          use_origco,
                                          fntrd.vert_positions,
                                          fntrd.faces,
                                          fntrd.corner_verts,
                                          fntrd.corner_tris,
                                          fntrd.hide_poly,
                                          fntrd.subdiv_ccg,
                                          fntrd.ray_start,
                                          fntrd.ray_normal,
                                          &fntrd.depth,
                                          &fntrd.dist_sq_to_ray))
  {
    fntrd.hit = true;
    *tmin = fntrd.dist_sq_to_ray;
  }
}

float raycast_init(ViewContext *vc,
                   const float2 &mval,
                   float3 &r_ray_start,
                   float3 &r_ray_end,
                   float3 &r_ray_normal,
                   bool original)
{
  Object &ob = *vc->obact;
  RegionView3D *rv3d = vc->rv3d;
  View3D *v3d = vc->v3d;

  /* TODO: what if the segment is totally clipped? (return == 0). */
  ED_view3d_win_to_segment_clipped(
      vc->depsgraph, vc->region, vc->v3d, mval, r_ray_start, r_ray_end, true);

  const float4x4 &world_to_object = ob.world_to_object();
  r_ray_start = math::transform_point(world_to_object, r_ray_start);
  r_ray_end = math::transform_point(world_to_object, r_ray_end);

  float dist;
  r_ray_normal = math::normalize_and_get_length(r_ray_end - r_ray_start, dist);

  if (rv3d->is_persp || RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    return dist;
  }

  /* Get the view origin without the addition
   * of -ray_normal * clip_start that
   * ED_view3d_win_to_segment_clipped gave us.
   * This is necessary to avoid floating point overflow.
   */
  float3 view_origin;
  ED_view3d_win_to_origin(vc->region, mval, view_origin);
  r_ray_start = math::transform_point(world_to_object, view_origin);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  bke::pbvh::clip_ray_ortho(pbvh, original, r_ray_start, r_ray_end, r_ray_normal);

  return math::distance(r_ray_start, r_ray_end);
}

std::optional<ActiveElementInfo> active_element_info_get(ViewContext &vc, const float2 &mval)
{
  Object &ob = *vc.obact;
  SculptSession &ss = *ob.runtime->sculpt_session;

  BKE_view_layer_synced_ensure(*vc.bmain, vc.scene, vc.view_layer);

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);

  if (!pbvh || !vc.rv3d ||
      !BKE_base_is_visible(vc.v3d, BKE_view_layer_base_find(vc.view_layer, &ob)))
  {
    return std::nullopt;
  }

  vert_random_access_ensure(ob);

  float3 ray_start;
  float3 ray_end;
  float3 ray_normal;
  float depth = raycast_init(&vc, mval, ray_start, ray_end, ray_normal, false);

  RaycastData srd{};
  srd.object = &ob;
  srd.ray_start = ray_start;
  srd.ray_normal = ray_normal;
  srd.hit = false;
  srd.depth = depth;

  srd.is_mid_stroke = false;
  srd.use_original = false;
  if (pbvh->type() == bke::pbvh::Type::Mesh) {
    const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
    srd.vert_positions = bke::pbvh::vert_positions_eval(*vc.depsgraph, ob);
    srd.faces = mesh.faces();
    srd.corner_verts = mesh.corner_verts();
    srd.corner_tris = mesh.corner_tris();
    const bke::AttributeAccessor attributes = mesh.attributes();
    srd.hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  }
  else if (pbvh->type() == bke::pbvh::Type::Grids) {
    srd.subdiv_ccg = ss.subdiv_ccg;
  }

  isect_ray_tri_watertight_v3_precalc(&srd.isect_precalc, ray_normal);
  bke::pbvh::raycast(
      *pbvh,
      [&](bke::pbvh::Node &node, float *tmin) { sculpt_raycast_cb(node, srd, tmin); },
      ray_start,
      ray_normal,
      srd.use_original);

  /* Cursor is not over the mesh, return default values. */
  if (!srd.hit) {
    return std::nullopt;
  }

  ActiveElementInfo info;
  info.vert = srd.active_vertex;
  switch (pbvh->type()) {
    case bke::pbvh::Type::Mesh:
      info.active_face_idx = srd.active_face_grid_index;
      break;
    case bke::pbvh::Type::Grids:
      info.active_grid_idx = srd.active_face_grid_index;
      break;
    case bke::pbvh::Type::BMesh:
      break;
  }
  return info;
}

std::optional<CursorGeometryInfo> cursor_geometry_info_update(bContext *C,
                                                              const float2 &mval,
                                                              const bool use_sampled_normal)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Base *base = CTX_data_active_base(C);

  return cursor_geometry_info_update(
      *depsgraph, sd.paint, &sd, vc, base, mval, use_sampled_normal);
}

std::optional<CursorGeometryInfo> cursor_geometry_info_update(Depsgraph &depsgraph,
                                                              const Paint &paint,
                                                              const Sculpt *sd,
                                                              ViewContext &vc,
                                                              const Base *base,
                                                              const float2 &mval,
                                                              const bool use_sampled_normal)
{
  const Brush &brush = *BKE_paint_brush_for_read(&paint);
  bool original = false;
  CursorGeometryInfo out;

  Object &ob = *vc.obact;
  SculptSession &ss = *ob.runtime->sculpt_session;

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);

  if (!pbvh || !vc.rv3d || !BKE_base_is_visible(vc.v3d, base)) {
    ss.clear_active_elements(false);
    return std::nullopt;
  }

  /* bke::pbvh::Tree raycast to get active vertex and face normal. */
  float3 ray_start;
  float3 ray_end;
  float3 ray_normal;
  float depth = raycast_init(&vc, mval, ray_start, ray_end, ray_normal, original);
  if (sd) {
    stroke_modifiers_check(depsgraph, vc.rv3d, *sd, ob, &brush);
  }

  RaycastData srd{};
  srd.use_original = original;
  srd.object = &ob;
  srd.is_mid_stroke = ob.runtime->sculpt_session->cache != nullptr;
  srd.hit = false;
  if (pbvh->type() == bke::pbvh::Type::Mesh) {
    const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
    srd.vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
    srd.faces = mesh.faces();
    srd.corner_verts = mesh.corner_verts();
    srd.corner_tris = mesh.corner_tris();
    const bke::AttributeAccessor attributes = mesh.attributes();
    srd.hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  }
  else if (pbvh->type() == bke::pbvh::Type::Grids) {
    srd.subdiv_ccg = ss.subdiv_ccg;
  }
  vert_random_access_ensure(ob);
  srd.ray_start = ray_start;
  srd.ray_normal = ray_normal;
  srd.depth = depth;

  isect_ray_tri_watertight_v3_precalc(&srd.isect_precalc, ray_normal);
  bke::pbvh::raycast(
      *pbvh,
      [&](bke::pbvh::Node &node, float *tmin) { sculpt_raycast_cb(node, srd, tmin); },
      ray_start,
      ray_normal,
      srd.use_original);

  /* Cursor is not over the mesh, return default values. */
  if (!srd.hit) {
    ss.clear_active_elements(true);
    return std::nullopt;
  }

  /* Update the active vertex of the SculptSession. */
  ss.set_active_vert(srd.active_vertex);

  switch (pbvh->type()) {
    case bke::pbvh::Type::Mesh:
      ss.active_face_index = srd.active_face_grid_index;
      ss.active_grid_index = std::nullopt;
      break;
    case bke::pbvh::Type::Grids:
      ss.active_face_index = std::nullopt;
      ss.active_grid_index = srd.active_face_grid_index;
      break;
    case bke::pbvh::Type::BMesh:
      ss.active_face_index = std::nullopt;
      ss.active_grid_index = std::nullopt;
      break;
  }

  out.location = ray_start + ray_normal * srd.depth;

  /* Option to return the face normal directly for performance o accuracy reasons. */
  if (!use_sampled_normal) {
    out.normal = srd.face_normal;
    return srd.hit ? std::make_optional(out) : std::nullopt;
  }

  /* Sampled normal calculation. */

  /* Update cursor data in SculptSession. */
  const float3 z_axis = {0.0f, 0.0f, 1.0f};
  ob.runtime->world_to_object = math::invert(ob.object_to_world());
  ss.cursor_view_normal = math::normalize(
      math::transform_direction(ob.world_to_object() * float4x4(vc.rv3d->viewinv), z_axis));
  ss.cursor_normal = srd.face_normal;
  ss.cursor_location = out.location;
  ss.rv3d = vc.rv3d;
  ss.v3d = vc.v3d;

  ss.cursor_radius = object_space_radius_get(vc, paint, brush, out.location);

  IndexMaskMemory memory;
  const IndexMask node_mask = pbvh_gather_cursor_update(ob, original, memory);

  /* In case there are no nodes under the cursor, return the face normal. */
  if (node_mask.is_empty()) {
    out.normal = srd.face_normal;
    return std::make_optional(out);
  }

  bke::pbvh::update_normals(depsgraph, ob, *pbvh);

  /* Calculate the sampled normal. */
  if (const std::optional<float3> sampled_normal = calc_area_normal(
          depsgraph, brush, ob, node_mask))
  {
    out.normal = *sampled_normal;
    ss.cursor_sampled_normal = *sampled_normal;
  }
  else {
    /* Use face normal when there are no vertices to sample inside the cursor radius. */
    out.normal = srd.face_normal;
  }
  return std::make_optional(out);
}

/**
 * \param check_closest: if true and the ray test fails a point closest to the ray will be found.
 * \param limit_closest_radius: if true then the closest point will be tested against the active
 * brush radius.
 */
static bool stroke_get_location_bvh_ex(Depsgraph &depsgraph,
                                       ViewContext &vc,
                                       const Paint &paint,
                                       const Sculpt *sd,
                                       float3 &out,
                                       const float2 &mval,
                                       const bool force_original,
                                       const bool check_closest,
                                       const bool limit_closest_radius)
{
  Object &ob = *vc.obact;

  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache *cache = ss.cache;
  const bool original = force_original || ((cache) ? !cache->accum : false);
  const Brush *brush = BKE_paint_brush_for_read(&paint);

  if (sd) {
    /* TODO: This code is shared by Sculpt, Vertex, and Weight paint. Ideally, we wouldn't need
     * to pass in `Sculpt` and `Paint` separately, but until we have further C++ DNA types, this
     * is fine */
    stroke_modifiers_check(depsgraph, vc.rv3d, *sd, ob, brush);
  }

  float3 ray_start;
  float3 ray_end;
  float3 ray_normal;
  const float depth = raycast_init(&vc, mval, ray_start, ray_end, ray_normal, original);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  bool hit = false;
  {
    RaycastData rd;
    rd.object = &ob;
    rd.is_mid_stroke = ob.runtime->sculpt_session->cache != nullptr;
    rd.ray_start = ray_start;
    rd.ray_normal = ray_normal;
    rd.hit = false;
    if (pbvh.type() == bke::pbvh::Type::Mesh) {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      rd.vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
      rd.faces = mesh.faces();
      rd.corner_verts = mesh.corner_verts();
      rd.corner_tris = mesh.corner_tris();
      const bke::AttributeAccessor attributes = mesh.attributes();
      rd.hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
    }
    else if (pbvh.type() == bke::pbvh::Type::Grids) {
      rd.subdiv_ccg = ss.subdiv_ccg;
    }
    vert_random_access_ensure(ob);
    rd.depth = depth;
    rd.use_original = original;
    isect_ray_tri_watertight_v3_precalc(&rd.isect_precalc, ray_normal);

    bke::pbvh::raycast(
        pbvh,
        [&](bke::pbvh::Node &node, float *tmin) { sculpt_raycast_cb(node, rd, tmin); },
        ray_start,
        ray_normal,
        rd.use_original);
    if (rd.hit) {
      hit = true;
      out = ray_start + ray_normal * rd.depth;
    }
  }

  if (hit || !check_closest) {
    return hit;
  }

  FindNearestToRayData fntrd{};
  fntrd.use_original = original;
  fntrd.object = &ob;
  fntrd.is_mid_stroke = ss.cache != nullptr;
  fntrd.hit = false;
  if (pbvh.type() == bke::pbvh::Type::Mesh) {
    const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
    fntrd.vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
    fntrd.faces = mesh.faces();
    fntrd.corner_verts = mesh.corner_verts();
    fntrd.corner_tris = mesh.corner_tris();
    const bke::AttributeAccessor attributes = mesh.attributes();
    fntrd.hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  }
  else if (pbvh.type() == bke::pbvh::Type::Grids) {
    fntrd.subdiv_ccg = ss.subdiv_ccg;
  }
  fntrd.ray_start = ray_start;
  fntrd.ray_normal = ray_normal;
  fntrd.depth = std::numeric_limits<float>::max();
  fntrd.dist_sq_to_ray = std::numeric_limits<float>::max();

  bke::pbvh::find_nearest_to_ray(
      pbvh,
      [&](bke::pbvh::Node &node, float *tmin) {
        sculpt_find_nearest_to_ray_cb(node, fntrd, tmin);
      },
      ray_start,
      ray_normal,
      fntrd.use_original);
  if (fntrd.hit && fntrd.dist_sq_to_ray) {
    hit = true;
    out = ray_start + ray_normal * fntrd.depth;
  }

  float closest_radius_sq = std::numeric_limits<float>::max();
  if (limit_closest_radius) {
    if (brush) {
      closest_radius_sq = object_space_radius_get(vc, paint, *brush, out);
      closest_radius_sq *= closest_radius_sq;
    }
  }

  return hit && fntrd.dist_sq_to_ray < closest_radius_sq;
}

bool stroke_get_location_bvh(Depsgraph &depsgraph,
                             ViewContext &vc,
                             const Sculpt &sd,
                             const Brush *brush,
                             float out[3],
                             const float mval[2],
                             const bool force_original)
{
  const bool check_closest = brush && brush->falloff_shape == PAINT_FALLOFF_SHAPE_TUBE;

  float3 location;
  const bool result = stroke_get_location_bvh_ex(
      depsgraph, vc, sd.paint, &sd, location, mval, force_original, check_closest, true);
  if (result) {
    copy_v3_v3(out, location);
  }
  return result;
}

bool stroke_get_location_bvh(Depsgraph &depsgraph,
                             ViewContext &vc,
                             const Paint &paint,
                             const Brush *brush,
                             float out[3],
                             const float mval[2],
                             const bool force_original)
{
  const bool check_closest = brush && brush->falloff_shape == PAINT_FALLOFF_SHAPE_TUBE;

  float3 location;
  const bool result = stroke_get_location_bvh_ex(
      depsgraph, vc, paint, nullptr, location, mval, force_original, check_closest, true);
  if (result) {
    copy_v3_v3(out, location);
  }
  return result;
}

bool stroke_get_location_bvh(bContext *C,
                             float out[3],
                             const float mval[2],
                             const bool force_original)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Brush *brush = BKE_paint_brush(BKE_paint_get_active_from_context(C));

  return stroke_get_location_bvh(*depsgraph, vc, sd, brush, out, mval, force_original);
}

/* SCULPT_DRAW_TRANSPORT_V01: only explicit opt-in Draw/Anchored uses this path. */
#include "sculpt_draw_transport_v01.hh"

struct SculptPaintStroke final : public PaintStroke {
  std::unique_ptr<draw_transport_v01::State> draw_transport;
  int draw_transport_event = 0;
  Main *bmain_;
  Sculpt *sculpt_;
  Base *base_;
  PaintModeSettings *paint_mode_settings_;

  /* Needed to tag other viewports */
  wmWindowManager *wm_;

  SculptPaintStroke(bContext *C, wmOperator *op, const wmEvent *event) : PaintStroke(C, op, event)
  {
    bmain_ = CTX_data_main(C);
    if (event && brush && (draw_transport_v01::enabled(*brush) ||
                          RNA_boolean_get(op->ptr, "draw_transport"))) {
      draw_transport = std::make_unique<draw_transport_v01::State>();
      draw_transport_event = event->type;
    }

    ToolSettings *tool_settings = CTX_data_tool_settings(C);
    sculpt_ = tool_settings->sculpt;
    paint_mode_settings_ = &tool_settings->paint_mode;
    base_ = CTX_data_active_base(C);
    wm_ = CTX_wm_manager(C);
  }

  void stroke_cache_init(const float mval[2]);
  void stroke_cache_update(PointerRNA *ptr);

  bool get_location(float out[3], const float mouse[2], bool force_original) override;
  bool test_start(wmOperator *op, const float mouse[2]) override;
  void redraw(bool final) override;
  bool test_cancel() override;
  void update_step(wmOperator *op, PointerRNA *itemptr) override;
  void done(bool is_cancel, bool stroke_started) override;
};

bool SculptPaintStroke::get_location(float out[3], const float mouse[2], bool force_original)
{
  return stroke_get_location_bvh(
      *this->depsgraph, this->vc, *sculpt_, this->brush, out, mouse, force_original);
}

static void brush_init_tex(const Sculpt &sd, SculptSession &ss)
{
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
  const MTex *mask_tex = BKE_brush_mask_texture_get(brush, OB_MODE_SCULPT);

  /* Init mtex nodes. */
  if (mask_tex->tex && mask_tex->tex->nodetree) {
    /* Has internal flag to detect it only does it once. */
    ntreeTexBeginExecTree(mask_tex->tex->nodetree);
  }

  if (ss.tex_pool == nullptr) {
    ss.tex_pool = BKE_image_pool_new();
  }
}

/** Creates stroke-level toggle settings, modifies the current active brush if needed */
static StrokeToggleSettings create_toggle_settings(const wmOperator &op, Main &bmain, Paint &paint)
{
  const BrushStrokeMode stroke_mode = BrushStrokeMode(RNA_enum_get(op.ptr, "mode"));
  const BrushSwitchMode brush_switch_mode = BrushSwitchMode(RNA_enum_get(op.ptr, "brush_toggle"));
  const bool pen_flip = RNA_boolean_get(op.ptr, "pen_flip");

  StrokeToggleSettings toggle_settings;

  toggle_settings.invert = stroke_mode == BrushStrokeMode::Invert || pen_flip;
  toggle_settings.alt_smooth = brush_switch_mode == BrushSwitchMode::Smooth;
  toggle_settings.alt_mask = brush_switch_mode == BrushSwitchMode::Mask;

  /* Alt-Smooth. */
  if (toggle_settings.alt_smooth) {
    smooth_brush_toggle_on(&bmain, &paint, toggle_settings);
  }
  /* Alt-Mask. */
  if (toggle_settings.alt_mask) {
    mask_brush_toggle_on(&bmain, &paint, toggle_settings);
  }
  return toggle_settings;
}

static void brush_stroke_init(bContext *C, const wmOperator *op)
{
  blender::bke::sculpt_cold_v01::Scope cold_trace("sculpt.brush_init", CTX_data_active_object(C));
  Object &ob = *CTX_data_active_object(C);
  ToolSettings *tool_settings = CTX_data_tool_settings(C);
  Sculpt &sd = *tool_settings->sculpt;
  SculptSession &ss = *CTX_data_active_object(C)->runtime->sculpt_session;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  if (!G.background) {
    view3d_operator_needs_gpu(C);
  }

  if (!ss.cache) {
    ss.cache = MEM_new<StrokeCache>(__func__);
    ss.cache->toggle_settings = create_toggle_settings(*op, *CTX_data_main(C), sd.paint);
  }

  cold_trace.mark("gpu_context_and_stroke_cache");
  brush_init_tex(sd, ss);
  cold_trace.mark("brush_textures");

  const bool needs_colors = brush_type_is_paint(brush->sculpt_brush_type) &&
                            !SCULPT_use_image_paint_brush(tool_settings->paint_mode, ob);

  if (needs_colors) {
    BKE_sculpt_color_layer_create_if_needed(&ob);
  }

  /* CTX_data_ensure_evaluated_depsgraph should be used at the end to include the updates of
   * earlier steps modifying the data. */
  cold_trace.mark("color_layer");
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  cold_trace.mark("ensure_evaluated_depsgraph");
  BKE_sculpt_update_object_for_edit(depsgraph, &ob, brush_type_is_paint(brush->sculpt_brush_type));

  cold_trace.mark("sculpt_object_for_edit");
  ED_paint_brush_type_update_sticky_shading_color(C, &ob);
}

static void restore_from_undo_step_if_necessary(const Depsgraph &depsgraph,
                                                const Sculpt &sd,
                                                Object &ob)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *ob.runtime->sculpt_session;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  /* Brushes that use original coordinates and need a "restore" step. This has to happen separately
   * rather than in the brush deformation calculation because that is called once for each symmetry
   * pass, potentially within the same BVH node.
   *
   * NOTE: Despite the Cloth and Boundary brush using original coordinates, the brushes do not
   * expect this restoration to happen on every stroke step. Performing this restoration causes
   * issues with the cloth simulation mode for those brushes.
   */
  if (ELEM(brush->sculpt_brush_type,
           SCULPT_BRUSH_TYPE_ELASTIC_DEFORM,
           SCULPT_BRUSH_TYPE_GRAB,
           SCULPT_BRUSH_TYPE_THUMB,
           SCULPT_BRUSH_TYPE_ROTATE))
  {
    undo::restore_from_undo_step(depsgraph, sd, ob);
    return;
  }

  /* For the cloth brush it makes more sense to not restore the mesh state to keep running the
   * simulation from the previous state. */
  if (brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH) {
    return;
  }

  /* Restore the mesh before continuing with anchored stroke. */
  if (ELEM(brush->stroke_method, BRUSH_STROKE_ANCHORED, BRUSH_STROKE_DRAG_DOT)) {

    undo::restore_from_undo_step(depsgraph, sd, ob);

    if (ss.cache) {
      /* Temporary data within the StrokeCache that is usually cleared at the end of the stroke
       * needs to be invalidated here so that the brushes do not accumulate and apply extra data.
       * See #129069. */
      ss.cache->layer_displacement_factor = {};
      ss.cache->paint_brush.mix_colors = {};
    }
  }
}

static void tag_mesh_positions_changed(Object &object, const bool use_pbvh_draw)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);

  /* Various operations inside sculpt mode can cause either the #MeshRuntimeData or the entire
   * Mesh to be changed (e.g. Undoing the very first operation after opening a file, performing
   * remesh, etc).
   *
   * This isn't an ideal fix for the core issue here, but to mitigate the drastic performance
   * falloff, we refreeze the cache before we do any operation that would tag this runtime
   * cache as dirty.
   *
   * See #130636. */
  if (!mesh.runtime->corner_tris_cache.frozen) {
    mesh.runtime->corner_tris_cache.freeze();
  }

  /* Updating mesh positions without marking caches dirty is generally not good, but since
   * sculpt mode has special requirements and is expected to have sole ownership of the mesh it
   * modifies, it's generally okay. */
  if (use_pbvh_draw) {
    /* When drawing from bke::pbvh::Tree is used, vertex and face normals are updated
     * later in #bke::pbvh::update_normals. However, we update the mesh's bounds eagerly here
     * since they are trivial to access from the bke::pbvh::Tree. Updating the
     * object's evaluated geometry bounding box is necessary because sculpt strokes don't cause
     * an object reevaluation. */
    mesh.tag_positions_changed_no_normals();
    /* Sculpt mode does not use or recalculate face corner normals, so they are cleared. */
    mesh.runtime->corner_normals_cache.tag_dirty();
  }
  else {
    /* Drawing happens from the modifier stack evaluation result.
     * Tag both coordinates and normals as modified, as both needed for proper drawing and the
     * modifier stack is not guaranteed to tag normals for update. */
    mesh.tag_positions_changed();
  }

  if (const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object)) {
    mesh.bounds_set_eager(bke::pbvh::bounds_get(*pbvh));
    if (object.runtime->bounds_eval) {
      object.runtime->bounds_eval = mesh.bounds_min_max();
    }
  }
}

void flush_update_step(bContext *C, const UpdateType update_type)
{
  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));
  flush_update_step(vc, *CTX_data_active_object(C), update_type);
}

void flush_update_step(ViewContext &vc, Object &object, const UpdateType update_type)
{
  PRF_scope(ProfileCategory::Editor);
  /* Sculpt Mesh V3 memory pressure probe: post-brush update / redraw scheduling. */
  const Mesh *sculpt_mesh_v3_flush_mesh = BKE_object_get_original_mesh(&object);
  const bool sculpt_mesh_v3_flush_enabled =
      (sculpt_mesh_v3_flush_mesh && sculpt_mesh_v3_flush_mesh->verts_num >= 1'000'000) ||
      (std::getenv("BLENDER_SCULPT_STROKE_PROFILE_LOG") &&
       std::getenv("BLENDER_SCULPT_STROKE_PROFILE_LOG")[0]);
  const SculptMeshV3MemorySample sculpt_mesh_v3_flush_mem_start =
      sculpt_mesh_v3_memory_sample();
  const double sculpt_mesh_v3_flush_start = BLI_time_now_seconds();

  struct SculptMeshV3FlushProbeScope {
    bool enabled;
    SculptMeshV3MemorySample start_memory;
    double start_time;

    ~SculptMeshV3FlushProbeScope()
    {
      if (!enabled) {
        return;
      }
      const double total_ms = (BLI_time_now_seconds() - start_time) * 1000.0;
      const SculptMeshV3MemorySample end_memory = sculpt_mesh_v3_memory_sample();
      const uint64_t page_fault_delta =
          (start_memory.valid && end_memory.valid &&
           end_memory.page_faults >= start_memory.page_faults) ?
              end_memory.page_faults - start_memory.page_faults :
              0;
      if (total_ms < 0.50 && page_fault_delta == 0) {
        return;
      }
      constexpr double mb = 1024.0 * 1024.0;
      std::fprintf(stderr,
                   "[SculptMeshV3Flush] total=%.3f ms pf=+%llu ws=%.1f MB "
                   "private=%.1f MB avail=%.1f MB\n",
                   total_ms,
                   static_cast<unsigned long long>(page_fault_delta),
                   end_memory.valid ? double(end_memory.working_set) / mb : 0.0,
                   end_memory.valid ? double(end_memory.private_bytes) / mb : 0.0,
                   end_memory.avail_phys ? double(end_memory.avail_phys) / mb : 0.0);
    }
  } sculpt_mesh_v3_flush_probe_scope{
      sculpt_mesh_v3_flush_enabled,
      sculpt_mesh_v3_flush_mem_start,
      sculpt_mesh_v3_flush_start};
  if (vc.rv3d) {
    /* Mark for faster 3D viewport redraws. */
    vc.rv3d->rflag |= RV3D_PAINTING;
  }

  const SculptSession &ss = *object.runtime->sculpt_session;
  const MultiresModifierData *mmd = ss.multires_modifier;
  if (mmd != nullptr) {
    /* LAYER_CCG_DIRTY_DOMAINS_V01: preserve independent edit domains. */
    switch (update_type) {
      case UpdateType::Position:
        multires_mark_as_modified(vc.depsgraph, &object, MULTIRES_COORDS_MODIFIED);
        break;
      case UpdateType::Mask:
        multires_mark_as_modified(vc.depsgraph, &object, MULTIRES_MASK_MODIFIED);
        break;
      case UpdateType::Visibility:
        multires_mark_as_modified(vc.depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
        break;
      case UpdateType::Color:
      case UpdateType::Image:
      case UpdateType::FaceSet:
        break;
    }
  }

  if (update_type == UpdateType::Image) {
    ED_region_tag_redraw(vc.region);
    if (update_type == UpdateType::Image) {
      /* Early exit when only need to update the images. We don't want to tag any geometry updates
       * that would rebuild the bke::pbvh::Tree. */
      return;
    }
  }

  DEG_id_tag_update(&object.id, ID_RECALC_SHADING);

  const bool use_pbvh_draw = BKE_sculptsession_use_pbvh_draw(&object, vc.rv3d);
  /* Only current viewport matters, slower update for all viewports will
   * be done in sculpt_flush_update_done. */
  if (!use_pbvh_draw) {
    /* Slow update with full dependency graph update and all that comes with it.
     * Needed when there are modifiers or full shading in the 3D viewport. */
    DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
  }

  ED_region_tag_redraw(vc.region);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (update_type == UpdateType::Position && !ss.shapekey_active) {
    if (pbvh.type() == bke::pbvh::Type::Mesh) {
      tag_mesh_positions_changed(object, use_pbvh_draw);
    }
  }
}

void flush_update_done(bContext *C, Object &ob, const UpdateType update_type)
{
  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));
  const wmWindowManager &wm = *CTX_wm_manager(C);
  flush_update_done(vc, wm, ob, update_type);
}

void flush_update_done(ViewContext &vc,
                       const wmWindowManager &wm,
                       Object &ob,
                       const UpdateType update_type)
{
  /* After we are done drawing the stroke, check if we need to do a more
   * expensive depsgraph tag to update geometry. */
  const Mesh &mesh = *id_cast<Mesh *>(ob.data);

  /* Always needed for linked duplicates. */
  bool need_tag = ID_REAL_USERS(&mesh.id) > 1;

  if (vc.rv3d) {
    vc.rv3d->rflag &= ~RV3D_PAINTING;
  }

  /* TODO: this might be better in the `redraw` callback instead of here */
  for (wmWindow &win : wm.windows) {
    const bScreen &screen = *WM_window_get_active_screen(&win);
    for (ScrArea &area : screen.areabase) {
      const SpaceLink &sl = *static_cast<SpaceLink *>(area.spacedata.first);
      if (sl.spacetype != SPACE_VIEW3D) {
        continue;
      }

      /* Tag all 3D viewports for redraw now that we are done. Other
       * viewports did not get a full redraw, and anti-aliasing for the
       * current viewport was deactivated. */
      for (ARegion &region : area.regionbase) {
        if (region.regiontype == RGN_TYPE_WINDOW) {
          const RegionView3D *other_rv3d = static_cast<RegionView3D *>(region.regiondata);
          if (other_rv3d != vc.rv3d) {
            need_tag |= !BKE_sculptsession_use_pbvh_draw(&ob, other_rv3d);
          }

          ED_region_tag_redraw(&region);
        }
      }
    }

    if (update_type == UpdateType::Image) {
      for (ScrArea &area : screen.areabase) {
        const SpaceLink &sl = *static_cast<SpaceLink *>(area.spacedata.first);
        if (sl.spacetype != SPACE_IMAGE) {
          continue;
        }
        ED_area_tag_redraw_regiontype(&area, RGN_TYPE_WINDOW);
      }
    }
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  if (update_type == UpdateType::Position) {
    bke::pbvh::store_bounds_orig(pbvh);

    /* Coordinates were modified, so fake neighbors are not longer valid. */
    fake_neighbors_free(ob);
  }

  if (update_type == UpdateType::Position) {
    if (pbvh.type() == bke::pbvh::Type::BMesh) {
      SculptSession &ss = *ob.runtime->sculpt_session;
      BKE_pbvh_bmesh_after_stroke(*ss.bm, pbvh);
    }
  }

  if (need_tag) {
    DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  }
}

/* Replace an entire attribute using implicit sharing to avoid copies when possible. */
static void replace_attribute(const bke::AttributeAccessor src_attributes,
                              const StringRef name,
                              const bke::AttrDomain domain,
                              const bke::AttrType data_type,
                              bke::MutableAttributeAccessor dst_attributes)
{
  dst_attributes.remove(name);
  bke::GAttributeReader src = src_attributes.lookup(name, domain, data_type);
  if (!src) {
    return;
  }
  if (src.sharing_info && src.varray.is_span()) {
    const bke::AttributeInitShared init(src.varray.get_internal_span().data(), *src.sharing_info);
    dst_attributes.add(name, domain, data_type, init);
  }
  else {
    const bke::AttributeInitVArray init(*src);
    dst_attributes.add(name, domain, data_type, init);
  }
}

static bool attribute_matches(const bke::AttributeAccessor a,
                              const bke::AttributeAccessor b,
                              const StringRef name)
{
  const bke::GAttributeReader a_attr = a.lookup(name);
  const bke::GAttributeReader b_attr = b.lookup(name);
  if (!a_attr.sharing_info || !b_attr.sharing_info) {
    return false;
  }
  return a_attr.sharing_info == b_attr.sharing_info;
}

static bool topology_matches(const Mesh &a, const Mesh &b)
{
  if (a.verts_num != b.verts_num || a.edges_num != b.edges_num || a.faces_num != b.faces_num ||
      a.corners_num != b.corners_num)
  {
    return false;
  }
  if (a.runtime->face_offsets_sharing_info != b.runtime->face_offsets_sharing_info) {
    return false;
  }
  const bke::AttributeAccessor a_attributes = a.attributes();
  const bke::AttributeAccessor b_attributes = b.attributes();
  if (!attribute_matches(a_attributes, b_attributes, ".edge_verts") ||
      !attribute_matches(a_attributes, b_attributes, ".corner_vert") ||
      !attribute_matches(a_attributes, b_attributes, ".corner_edge"))
  {
    return false;
  }
  return true;
}

static void store_sculpt_entire_mesh(const wmOperator &op,
                                     const Scene &scene,
                                     Object &object,
                                     Mesh *new_mesh)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  sculpt_paint::undo::geometry_begin(scene, object, &op);
  BKE_mesh_nomain_to_mesh(new_mesh, &mesh, &object, false);
  sculpt_paint::undo::geometry_end(object);
  BKE_sculptsession_free_pbvh(object);
}

static const ImplicitSharingInfo *get_vertex_group_sharing_info(const Mesh &mesh)
{
  const int layer_index = CustomData_get_layer_index(&mesh.vert_data, CD_MDEFORMVERT);
  if (layer_index == -1) {
    return nullptr;
  }
  return mesh.vert_data.layers[layer_index].sharing_info;
}

void store_mesh_from_eval(const wmOperator &op,
                          const Scene &scene,
                          const Depsgraph &depsgraph,
                          const RegionView3D *rv3d,
                          Object &object,
                          Mesh *new_mesh)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const bool changed_topology = !topology_matches(mesh, *new_mesh);
  const bool use_pbvh_draw = BKE_sculptsession_use_pbvh_draw(&object, rv3d);
  bool entire_mesh_changed = false;

  if (changed_topology) {
    store_sculpt_entire_mesh(op, scene, object, new_mesh);
    entire_mesh_changed = true;
  }
  else {
    /* Detect attributes present in the new mesh which no longer match the original. */
    VectorSet<StringRef> vertex_group_names;
    for (const bDeformGroup &vertex_group : mesh.vertex_group_names) {
      vertex_group_names.add(vertex_group.name);
    }

    VectorSet<StringRef> changed_attributes;
    new_mesh->attributes().foreach_attribute([&](const bke::AttributeIter &iter) {
      if (ELEM(iter.name, ".edge_verts", ".corner_vert", ".corner_edge")) {
        return;
      }
      if (vertex_group_names.contains(iter.name)) {
        /* Vertex group changes are handled separately. */
        return;
      }
      const bke::GAttributeReader attribute = iter.get();
      if (attribute_matches(new_mesh->attributes(), mesh.attributes(), iter.name)) {
        return;
      }
      changed_attributes.add(iter.name);
    });
    /* Detect attributes that were removed in the new mesh. */
    mesh.attributes().foreach_attribute([&](const bke::AttributeIter &iter) {
      if (!new_mesh->attributes().contains(iter.name)) {
        changed_attributes.add(iter.name);
      }
    });

    /* Vertex groups aren't handled fully by the attribute system, we need to use CustomData. */
    const bool vertex_groups_changed = get_vertex_group_sharing_info(mesh) !=
                                       get_vertex_group_sharing_info(*new_mesh);

    if (vertex_groups_changed) {
      changed_attributes.add_multiple(vertex_group_names);
    }

    /* Try to use the few specialized sculpt undo types that result in better performance, mainly
     * because redo avoids clearing the BVH, but also because some other updates can be skipped. */
    bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
    IndexMaskMemory memory;
    const IndexMask leaf_nodes = bke::pbvh::all_leaf_nodes(pbvh, memory);
    if (changed_attributes.as_span() == Span<StringRef>{"position"}) {
      undo::push_begin(scene, object, &op);
      undo::push_nodes(depsgraph, object, leaf_nodes, undo::Type::Position);
      undo::push_end(object);
      mesh.attribute_storage.wrap().remove("position");
      const bke::AttributeReader position = new_mesh->attributes().lookup<float3>("position");
      if (position.sharing_info) {
        /* Use lower level API to add the position attribute to avoid copying the array and to
         * allow using #tag_positions_changed_no_normals instead of #tag_positions_changed (which
         * would be called by the attribute API). */
        position.sharing_info->add_user();

        bke::Attribute::ArrayData data{};
        data.data = const_cast<float3 *>(position.varray.get_internal_span().data());
        data.size = position.varray.size();
        data.sharing_info = ImplicitSharingPtr<>(position.sharing_info);
        mesh.attribute_storage.wrap().add(
            "position", bke::AttrDomain::Point, bke::AttrType::Float3, std::move(data));
      }
      else {
        mesh.vert_positions_for_write().copy_from(VArraySpan(*position));
      }

      pbvh.tag_positions_changed(leaf_nodes);
      pbvh.update_bounds(depsgraph, object);
      tag_mesh_positions_changed(object, use_pbvh_draw);
      BKE_mesh_copy_parameters(&mesh, new_mesh);
      BKE_id_free(nullptr, new_mesh);
    }
    else if (changed_attributes.as_span() == Span<StringRef>{".sculpt_mask"}) {
      undo::push_begin(scene, object, &op);
      undo::push_nodes(depsgraph, object, leaf_nodes, undo::Type::Mask);
      undo::push_end(object);
      replace_attribute(new_mesh->attributes(),
                        ".sculpt_mask",
                        bke::AttrDomain::Point,
                        bke::AttrType::Float,
                        mesh.attributes_for_write());
      pbvh.tag_masks_changed(leaf_nodes);
      BKE_mesh_copy_parameters(&mesh, new_mesh);
      BKE_id_free(nullptr, new_mesh);
    }
    else if (changed_attributes.as_span() == Span<StringRef>{".sculpt_face_set"}) {
      undo::push_begin(scene, object, &op);
      undo::push_nodes(depsgraph, object, leaf_nodes, undo::Type::FaceSet);
      undo::push_end(object);
      replace_attribute(new_mesh->attributes(),
                        ".sculpt_face_set",
                        bke::AttrDomain::Face,
                        bke::AttrType::Int32,
                        mesh.attributes_for_write());
      pbvh.tag_face_sets_changed(leaf_nodes);
      BKE_mesh_copy_parameters(&mesh, new_mesh);
      BKE_id_free(nullptr, new_mesh);
    }
    else {
      /* Non-geometry-type sculpt undo steps can only handle a single change at a time. When
       * multiple attributes or attributes that don't have their own undo type are changed, we're
       * forced to fall back to the slower geometry undo type. */
      store_sculpt_entire_mesh(op, scene, object, new_mesh);
      entire_mesh_changed = true;
    }
  }
  DEG_id_tag_update(&mesh.id, ID_RECALC_SHADING);
  if (!use_pbvh_draw || entire_mesh_changed) {
    DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
  }
}

/* Returns whether the mouse/stylus is over the mesh (1)
 * or over the background (0). */
static bool over_mesh(bContext *C, wmOperator * /*op*/, const float mval[2])
{
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  const bool check_closest = brush->falloff_shape == PAINT_FALLOFF_SHAPE_TUBE;

  float3 co_dummy;
  return stroke_get_location_bvh_ex(
      *depsgraph, vc, sd.paint, &sd, co_dummy, mval, false, check_closest, true);
}

static bool over_mesh(Depsgraph &depsgraph,
                      ViewContext &vc,
                      const Sculpt &sd,
                      const Brush *brush,
                      wmOperator * /*op*/,
                      const float mval[2])
{
  const bool check_closest = brush->falloff_shape == PAINT_FALLOFF_SHAPE_TUBE;

  float3 co_dummy;
  return stroke_get_location_bvh_ex(
      depsgraph, vc, sd.paint, &sd, co_dummy, mval, false, check_closest, true);
}

static void stroke_undo_begin(const Scene &scene,
                              const Brush *brush,
                              PaintModeSettings &paint_mode_settings,
                              Object &object,
                              wmOperator *op)
{
  /* Setup the correct undo system. Image painting and sculpting are mutual exclusive.
   * Color attributes are part of the sculpting undo system. */
  if (brush && brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT &&
      SCULPT_use_image_paint_brush(paint_mode_settings, object))
  {
    ED_image_undo_push_begin(op->type->name, PaintMode::Sculpt);
  }
  else {
    undo::push_begin_ex(scene, object, sculpt_brush_type_name(*brush));
  }
}

static void stroke_undo_end(PaintModeSettings &paint_mode_settings, Object &object, Brush *brush)
{
  if (brush && brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT &&
      SCULPT_use_image_paint_brush(paint_mode_settings, object))
  {
    ED_image_undo_push_end();
  }
  else {
    undo::push_end(object);
  }
}

bool color_supported_check(const Scene &scene, Object &object, ReportList *reports)
{
  if (const SculptSession &ss = *object.runtime->sculpt_session; ss.bm) {
    BKE_report(reports, RPT_ERROR, "Not supported in dynamic topology mode");
    return false;
  }
  if (BKE_sculpt_multires_active(&scene, &object)) {
    BKE_report(reports, RPT_ERROR, "Not supported in multiresolution mode");
    return false;
  }

  return true;
}

/* TODO: `init` is a bad name */
void SculptPaintStroke::stroke_cache_init(const float mval[2])
{
  bke::PaintRuntime *paint_runtime = sculpt_->paint.runtime;
  const Brush *brush = this->brush;
  ViewContext *vc = &this->vc;
  Object &ob = *this->object;

  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache *cache = ss.cache;

  /* Set scaling adjustment. */
  float max_scale = 0.0f;
  for (int i = 0; i < 3; i++) {
    max_scale = max_ff(max_scale, fabsf(ob.scale[i]));
  }
  cache->scale[0] = max_scale / ob.scale[0];
  cache->scale[1] = max_scale / ob.scale[1];
  cache->scale[2] = max_scale / ob.scale[2];

  cache->plane_trim_squared = brush->plane_trim * brush->plane_trim;

  cache->mirror_modifier_clip.flag = 0;

  sculpt_init_mirror_clipping(ob, ss);

  /* Initial mouse location. */
  cache->initial_mouse = mval ? float2(mval) : float2(0.0f);

  cache->initial_location_symm = ss.cursor_location;
  cache->initial_location = ss.cursor_location;

  cache->initial_normal_symm = ss.cursor_sampled_normal.value_or(ss.cursor_normal);
  cache->initial_normal = ss.cursor_sampled_normal.value_or(ss.cursor_normal);

  /* Not very nice, but with current events system implementation
   * we can't handle brush appearance inversion hotkey separately (sergey). */
  if (cache->toggle_settings.invert) {
    paint_runtime->draw_inverted = true;
  }
  else {
    paint_runtime->draw_inverted = false;
  }

  cache->mouse = cache->initial_mouse;
  cache->mouse_event = cache->initial_mouse;
  copy_v2_v2(paint_runtime->tex_mouse, cache->initial_mouse);

  cache->initial_direction_flipped = brush_flip(*brush, *cache) < 0.0f;

  /* Truly temporary data that isn't stored in properties. */
  cache->vc = vc;
  cache->brush = brush;
  cache->paint = this->paint;

  /* Cache projection matrix. */
  cache->projection_mat = ED_view3d_ob_project_mat_get(cache->vc->rv3d, &ob);

  const float3 z_axis(0.0f, 0.0f, 1.0f);
  ob.runtime->world_to_object = math::invert(ob.object_to_world());
  cache->view_normal = math::normalize(math::transform_direction(
      ob.world_to_object() * float4x4(cache->vc->rv3d->viewinv), z_axis));
  cache->view_origin = math::transform_point(ob.world_to_object(),
                                             float3(cache->vc->rv3d->viewinv[3]));

  cache->supports_gravity = bke::brush::supports_gravity(*brush) && sculpt_->gravity_factor > 0.0f;
  /* Get gravity vector in world space. */
  if (cache->supports_gravity) {
    if (sculpt_->gravity_object) {
      const Object *gravity_object = sculpt_->gravity_object;
      cache->gravity_direction = gravity_object->object_to_world().z_axis();
    }
    else {
      cache->gravity_direction = {0.0f, 0.0f, 1.0f};
    }

    /* Transform to sculpted object space. */
    cache->gravity_direction = math::normalize(
        math::transform_direction(ob.world_to_object(), cache->gravity_direction));
  }

  cache->accum = true;

  /* Make copies of the mesh vertex locations and normals for some brushes. */
  if (brush->stroke_method == BRUSH_STROKE_ANCHORED) {
    cache->accum = false;
  }

  /* Draw sharp does not need the original coordinates to produce the accumulate effect, so it
   * should work the opposite way. */
  if (brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW_SHARP) {
    cache->accum = false;
  }

  if (bke::brush::supports_accumulate(*brush)) {
    if (!(brush->flag & BRUSH_ACCUMULATE)) {
      cache->accum = false;
      if (brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW_SHARP) {
        cache->accum = true;
      }
    }
  }

  /* Original coordinates require the sculpt undo system, which isn't used
   * for image brushes. It's also not necessary, just disable it. */
  if (brush && brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT &&
      SCULPT_use_image_paint_brush(*paint_mode_settings_, ob))
  {
    cache->accum = true;

    cache->image_data = paint::image::ImageData::init_active_image(
        ob, this->scene->toolsettings->paint_mode);
  }

  if (BKE_brush_color_jitter_get_settings(this->paint, brush)) {
    cache->initial_hsv_jitter = seed_hsv_jitter();
  }
  cache->first_time = true;
  cache->plane_brush.first_time = true;

  if (brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_ROTATE) {
    constexpr int pixel_input_threshold = 5;
    cache->dial = BLI_dial_init(cache->initial_mouse, pixel_input_threshold);
  }
}

bool SculptPaintStroke::test_start(wmOperator *op, const float mval[2])
{
  blender::bke::sculpt_cold_v01::Scope cold_trace("sculpt.test_start", this->object);
  /* Don't start the stroke until `mval` goes over the mesh. */
  if (over_mesh(*this->depsgraph, this->vc, *sculpt_, this->brush, op, mval)) {
    cold_trace.mark("raycast_over_mesh");
    Object &ob = *this->object;
    Brush *brush = this->brush;

    /* NOTE: This should be removed when paint mode is available. Paint mode can force based on the
     * canvas it is painting on. (ref. use_sculpt_texture_paint). */
    if (brush && brush_type_is_paint(brush->sculpt_brush_type) &&
        !SCULPT_use_image_paint_brush(*paint_mode_settings_, ob))
    {
      View3D *v3d = this->vc.v3d;
      if (v3d->shading.type == OB_SOLID) {
        v3d->shading.color_type = V3D_SHADING_VERTEX_COLOR;
      }
    }

    ED_view3d_init_mats_rv3d(&ob, this->vc.rv3d);

    stroke_cache_init(mval);
    if (brush && brush_type_is_paint(brush->sculpt_brush_type)) {
      BKE_curvemapping_init(brush->curve_rand_hue);
      BKE_curvemapping_init(brush->curve_rand_saturation);
      BKE_curvemapping_init(brush->curve_rand_value);
    }

    cold_trace.mark("stroke_cache_init");
    cursor_geometry_info_update(*this->depsgraph, *paint, sculpt_, this->vc, base_, mval, false);

    cold_trace.mark("cursor_geometry");
    stroke_undo_begin(*this->scene, this->brush, *this->paint_mode_settings_, *this->object, op);

    cold_trace.mark("undo_begin");
    return true;
  }
  return false;
}

/* Initialize the stroke cache variants from operator properties. */
void SculptPaintStroke::stroke_cache_update(PointerRNA *ptr)
{
  PRF_scope(ProfileCategory::Editor);
  const Depsgraph &depsgraph = *this->depsgraph;
  Paint &paint = *this->paint;
  bke::PaintRuntime &paint_runtime = *paint.runtime;
  SculptSession &ss = *this->object->runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  Brush &brush = *BKE_paint_brush(&paint);

  if (stroke_is_first_brush_step_of_symmetry_pass(cache) ||
      !((brush.stroke_method == BRUSH_STROKE_ANCHORED) ||
        (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SNAKE_HOOK) ||
        (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_ROTATE) ||
        cloth::is_cloth_deform_brush(brush)))
  {
    RNA_float_get_array(ptr, "location", cache.location);
  }

  RNA_float_get_array(ptr, "mouse", cache.mouse);
  RNA_float_get_array(ptr, "mouse_event", cache.mouse_event);

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SCENE_PROJECT) {
    init_scene_project_brush_targets(
        *this->depsgraph, *this->vc.view_layer, *this->vc.v3d, *this->object, cache);
  }

  /* XXX: Use pressure value from first brush step for brushes which don't support strokes (grab,
   * thumb). They depends on initial state and brush coord/pressure/etc.
   * It's more an events design issue, which doesn't split coordinate/pressure/angle changing
   * events. We should avoid this after events system re-design. */
  if (paint_supports_dynamic_size(brush, PaintMode::Sculpt) || cache.first_time) {
    cache.pressure = RNA_float_get(ptr, "pressure");
  }

  cache.tilt = {RNA_float_get(ptr, "x_tilt"), RNA_float_get(ptr, "y_tilt")};

  /* Truly temporary data that isn't stored in properties. */
  if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    cache.initial_radius = object_space_radius_get(*cache.vc, paint, brush, cache.location);

    if (!BKE_brush_use_locked_size(&paint, &brush)) {
      BKE_brush_unprojected_size_set(&paint, &brush, cache.initial_radius * 2.0f);
    }
  }

  /* Clay stabilized pressure. */
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLAY_THUMB) {
    if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
      ss.cache->clay_thumb_brush.pressure_stabilizer.fill(0.0f);
      ss.cache->clay_thumb_brush.stabilizer_index = 0;
    }
    else {
      cache.clay_thumb_brush.pressure_stabilizer[cache.clay_thumb_brush.stabilizer_index] =
          cache.pressure;
      cache.clay_thumb_brush.stabilizer_index += 1;
      if (cache.clay_thumb_brush.stabilizer_index >=
          ss.cache->clay_thumb_brush.pressure_stabilizer.size())
      {
        cache.clay_thumb_brush.stabilizer_index = 0;
      }
    }
  }

  if (BKE_brush_use_size_pressure(&brush) && paint_supports_dynamic_size(brush, PaintMode::Sculpt))
  {
    cache.radius = brush_dynamic_size_get(brush, cache, cache.initial_radius);
    cache.dyntopo_pixel_radius = brush_dynamic_size_get(
        brush, cache, paint_runtime.initial_pixel_radius);
  }
  else {
    cache.radius = cache.initial_radius;
    cache.dyntopo_pixel_radius = paint_runtime.initial_pixel_radius;
  }

  cache_paint_invariants_update(cache, brush);

  cache.radius_squared = cache.radius * cache.radius;

  if (brush.stroke_method == BRUSH_STROKE_ANCHORED) {
    /* True location has been calculated as part of the stroke system already here. */
    if (brush.flag & BRUSH_EDGE_TO_EDGE) {
      RNA_float_get_array(ptr, "location", cache.location);
    }

    cache.radius = paint_calc_object_space_radius(
        *cache.vc, cache.location, paint_runtime.pixel_radius);
    cache.radius_squared = cache.radius * cache.radius;
  }

  brush_delta_update(depsgraph, paint, *this->object, brush);

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_ROTATE) {
    cache.vertex_rotation = -BLI_dial_angle(cache.dial, cache.mouse) * cache.bstrength;

    paint_runtime.draw_anchored = true;
    copy_v2_v2(paint_runtime.anchored_initial_mouse, cache.initial_mouse);
    paint_runtime.anchored_size = paint_runtime.pixel_radius;
  }

  cache.special_rotation = paint_runtime.brush_rotation;

  cache.iteration_count++;
}

void SculptPaintStroke::update_step(wmOperator * /*op*/, PointerRNA *itemptr)
{
  blender::bke::sculpt_cold_v01::Scope cold_trace("sculpt.update_step", this->object);
  const Scene &scene = *this->scene;
  Depsgraph &depsgraph = *this->depsgraph;
  Sculpt &sd = *sculpt_;
  Object &ob = *this->object;
  SculptSession &ss = *ob.runtime->sculpt_session;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  StrokeCache *cache = ss.cache;
  if (blender::bke::sculpt_cold_v01::path()) {
    blender::bke::sculpt_cold_v01::event("DAB", "object=%p first=%d ccg=%p pbvh=%p brush=%d",
      static_cast<void *>(&ob), int(cache->first_time), static_cast<void *>(ss.subdiv_ccg),
      static_cast<void *>(bke::object::pbvh_get(ob)), int(this->brush->sculpt_brush_type));
  }
  cache->stroke_distance = this->stroke_distance();

  /* SCULPT_STROKE_PROFILE_V01: opt-in, one append per update; no geometry changes. */
  const char *stroke_profile_path = std::getenv("BLENDER_SCULPT_STROKE_PROFILE_LOG");
  const bool stroke_profile_enabled = stroke_profile_path && stroke_profile_path[0];
  const double stroke_profile_start = stroke_profile_enabled ? BLI_time_now_seconds() : 0.0;
  double stroke_profile_prepared = stroke_profile_start;
  double stroke_profile_brushed = stroke_profile_start;
  double stroke_profile_fixed = stroke_profile_start;


  stroke_modifiers_check(depsgraph, this->vc.rv3d, sd, ob, &brush);
  stroke_cache_update(itemptr);
  restore_from_undo_step_if_necessary(depsgraph, sd, ob);
  if (stroke_profile_enabled) { stroke_profile_prepared = BLI_time_now_seconds(); }

  if (dyntopo::stroke_is_dyntopo(ob, brush)) {
    do_symmetrical_brush_actions(
        depsgraph, scene, sd, ob, dynamic_topology_update, *this->paint_mode_settings_);
  }

  do_symmetrical_brush_actions(
      depsgraph, scene, sd, ob, do_brush_action, *this->paint_mode_settings_);

  if (stroke_profile_enabled) { stroke_profile_brushed = BLI_time_now_seconds(); }

  /* Hack to fix noise texture tearing mesh. */
  sculpt_fix_noise_tear(sd, ob);

  ss.cache->first_time = false;
  copy_v3_v3(ss.cache->last_location, ss.cache->location);

  if (stroke_profile_enabled) { stroke_profile_fixed = BLI_time_now_seconds(); }

  /* Cleanup. */
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
    flush_update_step(this->vc, *this->object, UpdateType::Mask);
  }
  else if (brush_type_is_paint(brush.sculpt_brush_type)) {
    if (SCULPT_use_image_paint_brush(*this->paint_mode_settings_, ob)) {
      flush_update_step(this->vc, *this->object, UpdateType::Image);
    }
    else {
      flush_update_step(this->vc, *this->object, UpdateType::Color);
    }
  }
  else {
    flush_update_step(this->vc, *this->object, UpdateType::Position);
  }
  if (stroke_profile_enabled) {
    const double finished = BLI_time_now_seconds();
    const SubdivCCG *ccg = ss.subdiv_ccg;
    const bool pbvh_draw = BKE_sculptsession_use_pbvh_draw(&ob, this->vc.rv3d);
    FILE *log = std::fopen(stroke_profile_path, "a");
    if (log) {
      std::fprintf(log,
                   "[SCULPT_STROKE] brush=%d grids=%d area=%d pbvh_draw=%d "
                   "prepare_ms=%.3f brush_symmetry_ms=%.3f tear_fix_ms=%.3f "
                   "flush_ms=%.3f callback_ms=%.3f\n",
                   int(brush.sculpt_brush_type), ccg ? ccg->grids_num : 0,
                   ccg ? ccg->grid_area : 0, int(pbvh_draw),
                   (stroke_profile_prepared - stroke_profile_start) * 1000.0,
                   (stroke_profile_brushed - stroke_profile_prepared) * 1000.0,
                   (stroke_profile_fixed - stroke_profile_brushed) * 1000.0,
                   (finished - stroke_profile_fixed) * 1000.0,
                   (finished - stroke_profile_start) * 1000.0);
      std::fclose(log);
    }
  }

}

static void brush_exit_tex(Sculpt &sd)
{
  Brush *brush = BKE_paint_brush(&sd.paint);
  const MTex *mask_tex = BKE_brush_mask_texture_get(brush, OB_MODE_SCULPT);

  if (mask_tex->tex && mask_tex->tex->nodetree) {
    ntreeTexEndExecTree(mask_tex->tex->nodetree->runtime->execdata);
  }
}

void SculptPaintStroke::done(bool is_cancel, bool stroke_started)
{
  const bool transport_profile=draw_transport && draw_transport->profile;
  const double transport_done_start=transport_profile?BLI_time_now_seconds():0.0;
  if (draw_transport) {
    ED_workspace_status_text(this->evil_C, nullptr);
    draw_transport.reset();
  }
  layer_delta_smooth_v01::reset();
  Object &ob = *this->object;
  SculptSession &ss = *ob.runtime->sculpt_session;
  Sculpt &sd = *this->sculpt_;

  /* Finished. */
  if (!ss.cache) {
    brush_exit_tex(sd);
    return;
  }
  bke::PaintRuntime *paint_runtime = sd.paint.runtime;
  Brush *brush = BKE_paint_brush(&sd.paint);
  paint_runtime->draw_inverted = false;

  stroke_modifiers_check(*this->depsgraph, this->vc.rv3d, sd, ob, brush);

  /* Alt-Smooth. */
  if (ss.cache->toggle_settings.alt_smooth) {
    smooth_brush_toggle_off(&sd.paint, ss.cache);
    /* Refresh the brush pointer in case we switched brush in the toggle function. */
    brush = BKE_paint_brush(&sd.paint);
  }
  /* Toggle Mask */
  if (ss.cache->toggle_settings.alt_mask) {
    mask_brush_toggle_off(&sd.paint, ss.cache);
    /* Refresh the brush pointer in case we switched brush in the toggle function. */
    brush = BKE_paint_brush(&sd.paint);
  }

  MEM_delete(ss.cache);
  ss.cache = nullptr;

  if (!is_cancel && stroke_started) {
    stroke_undo_end(*paint_mode_settings_, *this->object, brush);
  }

  if (brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
    flush_update_done(this->vc, *wm_, ob, UpdateType::Mask);
  }
  else if (brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT) {
    if (SCULPT_use_image_paint_brush(*this->paint_mode_settings_, ob)) {
      flush_update_done(this->vc, *wm_, ob, UpdateType::Image);
    }
    else {
      flush_update_done(this->vc, *wm_, ob, UpdateType::Color);
    }
  }
  else {
    flush_update_done(this->vc, *wm_, ob, UpdateType::Position);
  }

  WM_event_add_notifier(this->evil_C, NC_OBJECT | ND_DRAW, &ob);
  brush_exit_tex(sd);
  if (transport_profile) {
    std::printf("[DRAW_TRANSPORT_V07_END] cancel=%d done_ms=%.3f\n",int(is_cancel),
                (BLI_time_now_seconds()-transport_done_start)*1000.0);
    std::fflush(stdout);
  }
}

void SculptPaintStroke::redraw(bool /*final*/) {}

bool SculptPaintStroke::test_cancel()
{
  const Brush &brush = *BKE_paint_brush_for_read(this->paint);

  /* XXX Canceling strokes that way does not work with dynamic topology,
   *     user will have to do real undo for now. See #46456. */
  bool ret_val = !dyntopo::stroke_is_dyntopo(*this->object, brush);
  return ret_val;
}

bool sculpt_layers_editable(bContext *C, ReportList *reports)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return false;
  }
  MultiresModifierData *mmd = bke::multires_layers::base_modifier(*ob);
  if (!mmd) {
    mmd = get_multires_modifier(CTX_data_scene(C), ob, false);
  }
  if (!mmd || mmd->sculpt_layers.is_empty()) {
    return true;
  }
  if (mmd->sculptlvl == 0 && !(mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh)) {
    Mesh &mesh = *id_cast<Mesh *>(ob->data);
    if (!bke::multires_layers::base_remap_after_topology_edit(*mmd, mesh)) {
      BKE_report(reports, RPT_ERROR, "Cannot identify surviving base vertices for L0 layers");
      return false;
    }
    if (!bke::multires_layers::base_has_visible_layers(*mmd)) {
      return true;
    }
    if (!mmd->sculpt_layer_recording || !bke::multires_layers::can_record(*mmd, mesh)) {
      BKE_report(reports, RPT_ERROR, "Enable REC on a visible layer, or disable all layers to sculpt the base");
      return false;
    }
    return bke::multires_layers::base_deltas_ensure(*bke::multires_layers::active(*mmd), mesh.verts_num);
  }
  /* Base-mesh sculpting changes coarse coordinates through the native mesh
   * path. It must not depend on REC or a visible layer; layer deltas stay in
   * their own Multires grids. Do not treat an evaluated CCG as the base. */
  const SculptSession *ss = ob->runtime->sculpt_session;
  if (ss && ss->subdiv_ccg == nullptr && ss->bm == nullptr &&
      (mmd->sculptlvl == 0 || (mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh)))
  {
    return true;
  }
  /* With every layer hidden, the evaluated CCG is the native Multires base.
   * Record into base MDisps through native reshape, independently of REC. */
  if (!bke::multires_layers::base_has_visible_layers(*mmd)) {
    return true;
  }
  if (!mmd->sculpt_layer_recording ||
      !bke::multires_layers::can_record(*mmd, *id_cast<Mesh *>(ob->data))) {
    BKE_report(reports, RPT_ERROR,
               "Enable REC on a visible layer with non-zero strength and a valid sculpt "
               "level; disable Sculpt Base Mesh");
    return false;
  }
  return true;
}

/* POLYPAINT_V02B_NATIVE_ENABLE: Multires color paint edits the CCG color master,
 * not the geometric Sculpt Layers. REC must not gate this brush. The existing
 * color_supported_check() still rejects unsupported PBVH and Dyntopo modes. */
static bool sculpt_polypaint_cpu_enabled()
{
  const char *value = std::getenv("BLENDER_SCULPT_POLYPAINT_CPU");
  return value && value[0] == '1' && value[1] == '\0';
}

static bool sculpt_polypaint_skips_geometry_rec(bContext *C)
{
  if (!sculpt_polypaint_cpu_enabled()) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);
  ToolSettings *settings = CTX_data_tool_settings(C);
  if (!ob || !scene || !settings || !settings->sculpt) {
    return false;
  }
  const Brush *brush = BKE_paint_brush_for_read(&settings->sculpt->paint);
  if (!brush || brush->sculpt_brush_type != SCULPT_BRUSH_TYPE_PAINT) {
    return false;
  }
  const MultiresModifierData *mmd = BKE_sculpt_multires_active(scene, ob);
  return mmd && mmd->sculptlvl > 0;
}

/* Face Set ID painting does not require layer REC. Shift Relax, alternate Mask,
 * cloth and gravity remain guarded because they can modify geometry. */
static bool sculpt_face_sets_skips_geometry_rec(bContext *C, const wmOperator &op)
{
  const ToolSettings *settings = CTX_data_tool_settings(C);
  const Sculpt *sd = settings ? settings->sculpt : nullptr;
  const Brush *brush = sd ? BKE_paint_brush_for_read(&sd->paint) : nullptr;
  if (!brush || brush->sculpt_brush_type != SCULPT_BRUSH_TYPE_DRAW_FACE_SETS ||
      brush->deform_target == BRUSH_DEFORM_TARGET_CLOTH_SIM) {
    return false;
  }
  const BrushSwitchMode mode = BrushSwitchMode(RNA_enum_get(op.ptr, "brush_toggle"));
  if (mode == BrushSwitchMode::Smooth || mode == BrushSwitchMode::Mask ||
      (sd->gravity_factor > 0.0f && bke::brush::supports_gravity(*brush))) {
    return false;
  }
  const Object *object = CTX_data_active_object(C);
  const SculptSession *ss = object ? object->runtime->sculpt_session : nullptr;
  return object && (!ss || !ss->bm);
}

/* Mask painting/smoothing changes a separate scalar domain. In particular,
 * Shift on a Mask brush is mask smoothing, not geometric Delta Smooth. */
static bool sculpt_mask_skips_geometry_rec(bContext *C, const wmOperator &op)
{
  const ToolSettings *settings = CTX_data_tool_settings(C);
  const Sculpt *sd = settings ? settings->sculpt : nullptr;
  const Brush *brush = sd ? BKE_paint_brush_for_read(&sd->paint) : nullptr;
  if (!brush || brush->sculpt_brush_type != SCULPT_BRUSH_TYPE_MASK ||
      brush->deform_target == BRUSH_DEFORM_TARGET_CLOTH_SIM ||
      BrushSwitchMode(RNA_enum_get(op.ptr, "brush_toggle")) == BrushSwitchMode::Mask ||
      (sd->gravity_factor > 0.0f && bke::brush::supports_gravity(*brush))) {
    return false;
  }
  const Object *object = CTX_data_active_object(C);
  const SculptSession *ss = object ? object->runtime->sculpt_session : nullptr;
  return object && (!ss || !ss->bm);
}

/* POLYPAINT_MERGE_STABLE1904: A coarse master inherited from before
 * Subdivide cannot be migrated safely inside the first new brush dab. The
 * V0.2b code did an all-grid resample while Color Undo captured dense data,
 * which caused a UI lock in the reported Windows test. Refuse only this
 * unsupported transition; never discard or downsample color data. */
static bool sculpt_polypaint_prepare_stroke_master(bContext *C, wmOperator *op)
{
  if (!sculpt_polypaint_skips_geometry_rec(C)) {
    return true;
  }
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);
  MultiresModifierData *mmd = const_cast<MultiresModifierData *>(
      BKE_sculpt_multires_active(scene, ob));
  if (!bke::multires_layers::polypaint_lazy_master_enabled()) {
    if (mmd && (mmd->sculpt_color_attribute[0] || !mmd->sculpt_color_attributes.is_empty())) {
      BKE_report(op->reports, RPT_ERROR, "Named Polypaint masters require the lazy-master launch profile");
      return false;
    }
    if (mmd && mmd->sculpt_color_grids && mmd->sculpt_color_level > 0 &&
        mmd->sculpt_color_level < mmd->totlvl)
    {
      BKE_report(op->reports, RPT_WARNING,
                 "Multires Polypaint: master promotion after Subdivide pending; original color preserved");
      return false;
    }
    return true;
  }
  const Mesh *mesh = id_cast<Mesh *>(ob->data);
  const SculptSession *ss = ob->runtime->sculpt_session;
  const char *multi = std::getenv("BLENDER_SCULPT_POLYPAINT_MULTI_ATTRIBUTE");
  if (mmd && ((multi && multi[0] == '1') || mmd->sculpt_color_attribute[0] ||
              !mmd->sculpt_color_attributes.is_empty())) {
    const char *name = mesh->active_color_attribute;
    if (!name || !name[0]) {
      BKE_report(op->reports, RPT_ERROR, "Select a Color Attribute before painting");
      return false;
    }
    if (!mmd->sculpt_color_attribute[0] || !STREQ(name, mmd->sculpt_color_attribute)) {
      if (!(U.uiflag & USER_GLOBALUNDO) || U.undosteps <= 0 ||
          (ss && (ss->cache || ss->filter_cache))) {
        BKE_report(op->reports, RPT_ERROR, "Use the Polypaint attribute controls between strokes with Global Undo enabled");
        return false;
      }
      UndoStack *selection_stack = ED_undo_stack_get();
      if (!selection_stack || selection_stack->step_init ||
          !ED_undo_is_memfile_compatible(C)) {
        return false;
      }
      UndoStep *previous = selection_stack->step_active;
      ED_undo_push(C, "Before Polypaint Attribute Change");
      if (!selection_stack->step_active || selection_stack->step_active == previous ||
          selection_stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE) {
        return false;
      }
      if (!bke::multires_layers::polypaint_attribute_activate(*mmd, name)) {
        BKE_report(op->reports, RPT_ERROR, "Invalid Polypaint attribute directory");
        return false;
      }
      DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
      previous = selection_stack->step_active;
      ED_undo_push(C, "Prepare Polypaint Attribute");
      if (!selection_stack->step_active || selection_stack->step_active == previous ||
          selection_stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE) {
        BKE_report(op->reports, RPT_ERROR, "Polypaint attribute Undo checkpoint failed");
        return false;
      }
    }
  }
  if (!mmd || mmd->sculptlvl < 1 || mmd->sculptlvl > mmd->totlvl ||
      mmd->sculptlvl > 13 || (ss && (ss->cache || ss->filter_cache)))
  {
    BKE_report(op->reports, RPT_ERROR, "Cannot prepare Paint resolution during another sculpt transaction");
    return false;
  }
  if (mmd->sculpt_color_grids && mmd->sculpt_color_level >= mmd->sculptlvl &&
      mmd->sculpt_color_level <= 13 && mmd->sculpt_color_grids_num == mesh->corners_num)
  {
    return true;
  }

  MultiresModifierData staged{};
  const int previous_level = mmd->sculpt_color_level;
  if (!bke::multires_layers::polypaint_stage_master(*mmd, mmd->sculptlvl,
                                                  mesh->corners_num, staged))
  {
    bke::multires_layers::polypaint_clear(staged);
    BKE_report(op->reports, RPT_ERROR, "Invalid Paint master; original colors preserved");
    return false;
  }
  const bool use_undo = U.undosteps > 0 && (U.uiflag & USER_GLOBALUNDO);
  UndoStack *stack = use_undo ? ED_undo_stack_get() : nullptr;
  if (use_undo && (!stack || stack->step_init || !ED_undo_is_memfile_compatible(C))) {
    bke::multires_layers::polypaint_clear(staged);
    BKE_report(op->reports, RPT_ERROR, "Paint promotion requires a completed Undo transaction");
    return false;
  }
  /* A full color-resolution change cannot cross older Color steps silently:
   * their saved grid strides differ. Preserve the old master in a Memfile
   * boundary before publishing. This happens once, outside stroke/parallel
   * node capture, and does not change Position Undo or first-stroke anchors. */
  if (use_undo) {
    UndoStep *before = stack->step_active;
    ED_undo_push(C, "Before Polypaint Resolution Change");
    if (!stack->step_active || stack->step_active == before ||
        stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE)
    {
      bke::multires_layers::polypaint_clear(staged);
      BKE_report(op->reports, RPT_ERROR, "Paint promotion checkpoint failed; original colors preserved");
      return false;
    }
  }
  bke::multires_layers::polypaint_swap_master(*mmd, staged);
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  if (use_undo) {
    UndoStep *before = stack->step_active;
    ED_undo_push(C, "Prepare Polypaint Resolution");
    if (!stack->step_active || stack->step_active == before ||
        stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE)
    {
      bke::multires_layers::polypaint_swap_master(*mmd, staged);
      bke::multires_layers::polypaint_clear(staged);
      DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
      BKE_report(op->reports, RPT_ERROR, "Paint promotion Undo failed; original colors restored");
      return false;
    }
  }
  bke::multires_layers::polypaint_clear(staged);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  const char *trace = std::getenv("BLENDER_LAYER_RESP_TRACE");
  if (trace && trace[0] && trace[0] != '0') {
    std::printf("[POLYPAINT_LAZY_MASTER] from=%d to=%d geometry=%d undo_boundary=%d\n",
                previous_level, mmd->sculpt_color_level, mmd->totlvl, int(use_undo));
    std::fflush(stdout);
  }
  return true;
}

static wmOperatorStatus sculpt_brush_stroke_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  blender::bke::sculpt_cold_v01::Scope cold_trace("sculpt.invoke", CTX_data_active_object(C));
  if (!sculpt_polypaint_prepare_stroke_master(C, op)) {
    return OPERATOR_CANCELLED;
  }

  const Sculpt *transport_sd = CTX_data_tool_settings(C)->sculpt;
  const Brush *transport_brush = transport_sd ? BKE_paint_brush_for_read(&transport_sd->paint) : nullptr;
  const bool transport_requested = transport_brush &&
      (draw_transport_v01::enabled(*transport_brush) || RNA_boolean_get(op->ptr, "draw_transport"));
  if (transport_requested) {
    const char *transport_flag = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_V01");
    if (!transport_flag || transport_flag[0] != '1' || transport_flag[1] != '\0') {
      BKE_report(op->reports, RPT_ERROR, "Use the Draw Transport V01 launch profile");
      return OPERATOR_CANCELLED;
    }
    if (const char *error = draw_transport_v01::preflight(C, *transport_brush, *op)) {
      BKE_report(op->reports, RPT_ERROR, error);
      return OPERATOR_CANCELLED;
    }
  }
  cold_trace.mark("polypaint_preflight");
  SculptPaintStroke *stroke;
  const bool non_geometric_brush = sculpt_face_sets_skips_geometry_rec(C, *op) ||
                                   sculpt_mask_skips_geometry_rec(C, *op);
  if (!sculpt_polypaint_skips_geometry_rec(C) && !non_geometric_brush &&
      !sculpt_layers_editable(C, op->reports))
  {
    return OPERATOR_CANCELLED;
  }
  const Brush *delta_brush = BKE_paint_brush_for_read(&CTX_data_tool_settings(C)->sculpt->paint);
  if (non_geometric_brush) { layer_delta_smooth_v01::reset(); }
  else if (delta_brush && !layer_delta_smooth_v01::begin(
          *CTX_data_active_object(C), *delta_brush, op->reports,
          BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle")) == BrushSwitchMode::Smooth))
  {
    return OPERATOR_CANCELLED;
  }

  cold_trace.mark("layer_checks_and_delta_smooth_begin");
  int ignore_background_click;
  Object &ob = *CTX_data_active_object(C);
  Scene &scene = *CTX_data_scene(C);
  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  /* Test that ob is visible; otherwise we won't be able to get evaluated data
   * from the depsgraph. We do this here instead of SCULPT_mode_poll
   * to avoid falling through to the translate operator in the
   * global view3d keymap. */
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  bool pen_flip;
  WM_event_tablet_data(event, &pen_flip, nullptr);

  stroke = MEM_new<SculptPaintStroke>(__func__, C, op, event);
  cold_trace.mark("stroke_constructor_including_depsgraph");
  brush_stroke_init(C, op);
  cold_trace.mark("brush_stroke_init");

  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  Brush &brush = *BKE_paint_brush(&sd.paint);

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT &&
      BKE_sculpt_multires_active(&scene, &ob) && !sculpt_polypaint_cpu_enabled())
  {
    BKE_report(op->reports, RPT_ERROR, "Experimental Multires Polypaint is disabled");
    stroke->cancel(C);
    MEM_delete(stroke);
    return OPERATOR_CANCELLED;
  }

  /* POLYPAINT_V02B_NATIVE_ENABLE: Only Paint has a validated experimental CCG
   * CPU writer. Keep Smear and Blur under the existing Multires restriction. */
  const bke::pbvh::Tree *paint_pbvh = bke::object::pbvh_get(ob);
  const bool multires_polypaint_stroke =
      sculpt_polypaint_cpu_enabled() &&
      brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT &&
      BKE_sculpt_multires_active(&scene, &ob) && paint_pbvh &&
      paint_pbvh->type() == bke::pbvh::Type::Grids;
  if (brush_type_is_paint(brush.sculpt_brush_type) && !multires_polypaint_stroke &&
      !color_supported_check(scene, ob, op->reports))
  {
    stroke->cancel(C);
    MEM_delete(stroke);
    return OPERATOR_CANCELLED;
  }
  if (!brush_type_is_attribute_only(brush.sculpt_brush_type) && !shape_key_check(ob, op->reports))
  {
    stroke->cancel(C);
    MEM_delete(stroke);
    return OPERATOR_CANCELLED;
  }
  if (ELEM(brush.sculpt_brush_type,
           SCULPT_BRUSH_TYPE_DISPLACEMENT_SMEAR,
           SCULPT_BRUSH_TYPE_DISPLACEMENT_ERASER))
  {
    const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
    if (!pbvh || pbvh->type() != bke::pbvh::Type::Grids) {
      BKE_report(op->reports, RPT_ERROR, "Only supported in multiresolution mode");
      stroke->cancel(C);
      MEM_delete(stroke);
      return OPERATOR_CANCELLED;
    }
  }

  if (brush_type_is_mask(brush.sculpt_brush_type)) {
    MultiresModifierData *mmd = BKE_sculpt_multires_active(&scene, &ob);
    BKE_sculpt_mask_layers_ensure(CTX_data_depsgraph_pointer(C), CTX_data_main(C), &ob, mmd);

    mask_overlay_check(*C, *op);
  }
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW_FACE_SETS) {
    face_set_overlay_check(*C, *op);
  }

  op->customdata = stroke;

  /* For tablet rotation. */
  ignore_background_click = RNA_boolean_get(op->ptr, "ignore_background_click");
  const float mval[2] = {float(event->mval[0]), float(event->mval[1])};
  if (ignore_background_click && !over_mesh(C, op, mval)) {
    stroke->cancel(C);
    MEM_delete(stroke);
    return OPERATOR_PASS_THROUGH;
  }

  cold_trace.mark("checks_before_first_modal");
  const wmOperatorStatus retval = op->type->modal(C, op, event);
  cold_trace.mark("first_modal_including_first_dab");
  OPERATOR_RETVAL_CHECK(retval);

  if (ELEM(retval, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    SculptPaintStroke *stroke = static_cast<SculptPaintStroke *>(op->customdata);
    if (stroke) {
      if (retval == OPERATOR_FINISHED) {
        stroke->finish(C);
      }
      else {
        stroke->cancel(C);
      }
      MEM_delete(stroke);
    }
    return retval;
  }
  /* Add modal handler. */
  WM_event_add_modal_handler(C, op);

  BLI_assert(retval == OPERATOR_RUNNING_MODAL);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus sculpt_brush_stroke_exec(bContext *C, wmOperator *op)
{
  Object *paint_object = CTX_data_active_object(C);
  const Sculpt *paint_sculpt = CTX_data_tool_settings(C)->sculpt;
  const Brush *paint_brush = paint_sculpt ? BKE_paint_brush_for_read(&paint_sculpt->paint) : nullptr;
  if (paint_object && paint_brush && paint_brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT &&
      BKE_sculpt_multires_active(CTX_data_scene(C), paint_object) &&
      !sculpt_polypaint_cpu_enabled())
  {
    BKE_report(op->reports, RPT_ERROR, "Experimental Multires Polypaint is disabled");
    return OPERATOR_CANCELLED;
  }
  if (!sculpt_polypaint_prepare_stroke_master(C, op)) {
    return OPERATOR_CANCELLED;
  }

  const bool non_geometric_brush = sculpt_face_sets_skips_geometry_rec(C, *op) ||
                                   sculpt_mask_skips_geometry_rec(C, *op);
  if (!sculpt_polypaint_skips_geometry_rec(C) && !non_geometric_brush &&
      !sculpt_layers_editable(C, op->reports))
  {
    return OPERATOR_CANCELLED;
  }
  const Brush *delta_brush = BKE_paint_brush_for_read(&CTX_data_tool_settings(C)->sculpt->paint);
  if (non_geometric_brush) { layer_delta_smooth_v01::reset(); }
  else if (delta_brush && !layer_delta_smooth_v01::begin(
          *CTX_data_active_object(C), *delta_brush, op->reports,
          BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle")) == BrushSwitchMode::Smooth))
  {
    return OPERATOR_CANCELLED;
  }

  brush_stroke_init(C, op);

  SculptPaintStroke *stroke = MEM_new<SculptPaintStroke>(__func__, C, op, nullptr);
  op->customdata = stroke;

  stroke->exec(C, op);

  MEM_delete(stroke);

  return OPERATOR_FINISHED;
}

static void sculpt_brush_stroke_cancel(bContext *C, wmOperator *op)
{
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  SculptPaintStroke *stroke = static_cast<SculptPaintStroke *>(op->customdata);

  BLI_assert(!dyntopo::stroke_is_dyntopo(ob, brush));
  UNUSED_VARS_NDEBUG(brush);

  undo::restore_from_undo_step(depsgraph, sd, ob);
  stroke->cancel(C);
}

static wmOperatorStatus brush_stroke_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  SculptPaintStroke *stroke = static_cast<SculptPaintStroke *>(op->customdata);
  if (stroke->draw_transport) {
    draw_transport_v01::State &transport = *stroke->draw_transport;
    if (transport.placing) {
      if ((ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE) && event->val == KM_PRESS) ||
          (event->type == EVT_MODAL_MAP && event->val == 1)) {
        sculpt_brush_stroke_cancel(C, op);
        MEM_delete(stroke);
        op->customdata = nullptr;
        return OPERATOR_CANCELLED;
      }
      if (event->type == EVT_RKEY && event->val == KM_PRESS &&
          !(event->flag & WM_EVENT_IS_REPEAT)) {
        if (!transport.valid && !transport.adjusting()) {
          BKE_report(op->reports, RPT_WARNING, "Place the detail on the surface before rotating");
          return OPERATOR_RUNNING_MODAL;
        }
        transport.rotating = !transport.rotating;
        transport.scaling = transport.strength_adjust = false;
        const float2 event_mouse(float(event->mval[0]),float(event->mval[1]));
        if (transport.rotating) {
          const float2 radial = event_mouse-transport.target_mouse;
          transport.rotation_base = transport.angle;
          transport.rotation_mouse_start = math::length_squared(radial) >= 4.0f ?
                                              std::atan2(radial.y,radial.x) : 0.0f;
          ED_workspace_status_text(C, "Draw Transport ROTATE: move around imprint center | Click/Enter/R: accept angle | Esc: cancel stroke");
        }
        else {
          transport.mouse_offset = transport.target_mouse-event_mouse;
          ED_workspace_status_text(C, "Draw Transport MOVE: mouse to position | R: rotate | S: scale / SS: strength | Click/Enter: confirm | Esc: cancel stroke");
        }
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->type == EVT_SKEY && event->val == KM_PRESS &&
          !(event->flag & WM_EVENT_IS_REPEAT)) {
        if (!transport.valid && !transport.adjusting()) {
          BKE_report(op->reports, RPT_WARNING, "Place the detail on the surface before adjusting");
          return OPERATOR_RUNNING_MODAL;
        }
        const float2 event_mouse(float(event->mval[0]),float(event->mval[1]));
        transport.rotating = false;
        if (transport.scaling) {
          /* Second S switches to strength; the current footprint scale remains frozen. */
          transport.scaling = false;
          transport.strength_adjust = true;
          transport.strength_base = transport.imprint_strength;
          transport.strength_mouse_start = event_mouse.x;
          ED_workspace_status_text(C, "Draw Transport STRENGTH: mouse left/right | Click/Enter: accept | Esc: cancel stroke");
        }
        else {
          transport.scaling = true;
          transport.strength_adjust = false;
          transport.scale_base = transport.footprint_scale;
          transport.scale_mouse_start = std::max(8.0f,math::length(event_mouse-transport.target_mouse));
          ED_workspace_status_text(C, "Draw Transport SCALE: move toward/away from center | S again: strength | Click/Enter: accept | Esc: cancel stroke");
        }
        return OPERATOR_RUNNING_MODAL;
      }
      if (ELEM(event->type, LEFTMOUSE, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS) {
        if (transport.adjusting()) {
          transport.rotating = transport.scaling = transport.strength_adjust = false;
          transport.mouse_offset = transport.target_mouse-
                                   float2(float(event->mval[0]),float(event->mval[1]));
          ED_workspace_status_text(C, "Draw Transport MOVE: mouse to position | R: rotate | S: scale / SS: strength | Click/Enter: confirm | Esc: cancel stroke");
          return OPERATOR_RUNNING_MODAL;
        }
        if (!transport.valid) {
          BKE_report(op->reports, RPT_WARNING, "Move the detail onto a valid surface before confirming");
          return OPERATOR_RUNNING_MODAL;
        }
        std::printf("[DRAW_TRANSPORT_V07] confirm\n");
        std::fflush(stdout);
        stroke->finish(C);
        MEM_delete(stroke);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      if (ISMOUSE_MOTION(event->type)) {
        bool moved = false;
        try { moved = draw_transport_v01::move(transport, C, stroke->vc, *event); }
        catch (const std::bad_alloc &) { transport.failure_reason = "Insufficient memory while positioning the imprint"; moved = false; }
        catch (const std::length_error &) { transport.failure_reason = "Imprint exceeds the representable buffer size"; moved = false; }
        if (!moved) {
          BKE_reportf(op->reports, RPT_ERROR, "Draw Transport: %s", transport.failure_reason ? transport.failure_reason : "Cannot position the imprint");
          sculpt_brush_stroke_cancel(C, op);
          MEM_delete(stroke);
          op->customdata = nullptr;
          return OPERATOR_CANCELLED;
        }
      }
      /* Keep the same open stroke/Undo transaction. No painting, navigation or mode changes. */
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->type == stroke->draw_transport_event && event->val == KM_RELEASE) {
      bool captured = false;
      try { captured = draw_transport_v01::capture(transport, *stroke->object); }
      catch (const std::bad_alloc &) { transport.failure_reason = "Insufficient memory while capturing the imprint"; captured = false; }
      catch (const std::length_error &) { transport.failure_reason = "Imprint exceeds the representable buffer size"; captured = false; }
      if (!captured) {
        BKE_reportf(op->reports, RPT_WARNING, "Draw Transport: %s; stroke cancelled", transport.failure_reason ? transport.failure_reason : "Cannot capture the imprint");
        sculpt_brush_stroke_cancel(C, op);
        MEM_delete(stroke);
        op->customdata = nullptr;
        return OPERATOR_CANCELLED;
      }
      transport.target_mouse = stroke->object->runtime->sculpt_session->cache->initial_mouse;
      transport.mouse_offset = transport.target_mouse-
                               float2(float(event->mval[0]), float(event->mval[1]));
      ED_workspace_status_text(C, "Draw Transport MOVE: move mouse to position | R: rotate | S: scale / SS: strength | Click/Enter: confirm | Esc/Right Click: cancel");
      ED_region_tag_redraw(CTX_wm_region(C));
      return OPERATOR_RUNNING_MODAL;
    }
  }
  const wmOperatorStatus retval = stroke->modal(C, op, event);

  if (ELEM(retval, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    MEM_delete(stroke);
    op->customdata = nullptr;
  }

  return retval;
}

static bool draw_transport_activate_poll_v03(bContext *C)
{
  return sculpt_mode_and_brush_poll(C);
}
static wmOperatorStatus draw_transport_activate_exec_v03(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || !ob->runtime->sculpt_session || ob->runtime->sculpt_session->cache ||
      ob->runtime->sculpt_session->filter_cache) {
    BKE_report(op->reports,RPT_ERROR,"Finish the current sculpt operation first");
    return OPERATOR_CANCELLED;
  }
  char name[MAX_ID_NAME];
  RNA_string_get(op->ptr,"brush_name",name);
  Brush *brush = reinterpret_cast<Brush *>(BKE_libblock_find_name(CTX_data_main(C),ID_BR,name));
  if (!brush || brush->id.lib || brush->sculpt_brush_type != SCULPT_BRUSH_TYPE_DRAW ||
      brush->stroke_method != BRUSH_STROKE_ANCHORED || !draw_transport_v01::enabled(*brush)) {
    BKE_report(op->reports,RPT_ERROR,"Choose the local Draw Transport brush created by the session UI");
    return OPERATOR_CANCELLED;
  }
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (!paint || !WM_toolsystem_activate_brush_and_tool(C,paint,brush)) {
    BKE_report(op->reports,RPT_ERROR,"Cannot activate Draw Transport in this tool context");
    return OPERATOR_CANCELLED;
  }
  WM_main_add_notifier(NC_ASSET | NA_ACTIVATED,nullptr);
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS,nullptr);
  return OPERATOR_FINISHED;
}
void SCULPT_OT_draw_transport_activate_v03(wmOperatorType *ot)
{
  ot->name = "Activate Draw Transport Brush";
  ot->idname = "SCULPT_OT_draw_transport_activate_v03";
  ot->description = "Activate the dedicated local Draw Transport brush";
  ot->poll = draw_transport_activate_poll_v03;
  ot->exec = draw_transport_activate_exec_v03;
  RNA_def_string(ot->srna,"brush_name","Draw Transport",MAX_ID_NAME,"Brush","Local transport brush name");
}

static void redo_empty_ui(bContext * /*C*/, wmOperator * /*op*/) {}

void SCULPT_OT_brush_stroke(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Sculpt";
  ot->idname = "SCULPT_OT_brush_stroke";
  ot->description = "Sculpt a stroke into the geometry";

  /* API callbacks. */
  ot->invoke = sculpt_brush_stroke_invoke;
  ot->modal = brush_stroke_modal;
  ot->exec = sculpt_brush_stroke_exec;
  ot->poll = sculpt_mode_and_brush_poll;
  ot->cancel = sculpt_brush_stroke_cancel;
  ot->ui = redo_empty_ui;

  /* Flags (sculpt does its own undo? (ton)). */
  ot->flag = OPTYPE_BLOCKING;

  /* Properties. */

  RNA_def_boolean(ot->srna, "draw_transport", false, "Draw Transport",
                  "Keep an Anchored Draw imprint movable until explicit confirmation");
  paint_stroke_operator_properties(ot);

  PropertyRNA *prop = RNA_def_boolean(
      ot->srna,
      "override_location",
      false,
      "Override Location",
      "Override the given \"location\" array by recalculating object space positions from the "
      "provided \"mouse_event\" positions");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "ignore_background_click",
                         false,
                         "Ignore Background Click",
                         "Clicks on the background do not start the stroke");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* Fake Neighbors. */

static void fake_neighbor_init(Object &object, const float max_dist)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const int totvert = vertex_count_get(object);
  ss.fake_neighbors.fake_neighbor_index = Array<int>(totvert, FAKE_NEIGHBOR_NONE);
  ss.fake_neighbors.current_max_distance = max_dist;
}

static void pose_fake_neighbors_free(SculptSession &ss)
{
  ss.fake_neighbors.fake_neighbor_index = {};
}

struct NearestVertData {
  int vert = -1;
  float distance_sq = std::numeric_limits<float>::max();

  static NearestVertData join(const NearestVertData &a, const NearestVertData &b)
  {
    NearestVertData joined = a;
    if (joined.vert == -1) {
      joined.vert = b.vert;
      joined.distance_sq = b.distance_sq;
    }
    else if (b.distance_sq < joined.distance_sq) {
      joined.vert = b.vert;
      joined.distance_sq = b.distance_sq;
    }
    return joined;
  }
};

static void fake_neighbor_search_mesh(const SculptSession &ss,
                                      const Span<float3> vert_positions,
                                      const Span<bool> hide_vert,
                                      const float3 &location,
                                      const float max_distance_sq,
                                      const int island_id,
                                      const bke::pbvh::MeshNode &node,
                                      NearestVertData &nvtd)
{
  for (const int vert : node.verts()) {
    if (!hide_vert.is_empty() && hide_vert[vert]) {
      continue;
    }
    if (ss.fake_neighbors.fake_neighbor_index[vert] != FAKE_NEIGHBOR_NONE) {
      continue;
    }
    if (islands::vert_id_get(ss, vert) == island_id) {
      continue;
    }
    const float distance_sq = math::distance_squared(vert_positions[vert], location);
    if (distance_sq < max_distance_sq && distance_sq < nvtd.distance_sq) {
      nvtd.vert = vert;
      nvtd.distance_sq = distance_sq;
    }
  }
}

static void fake_neighbor_search_grids(const SculptSession &ss,
                                       const CCGKey &key,
                                       const Span<float3> positions,
                                       const BitGroupVector<> &grid_hidden,
                                       const float3 &location,
                                       const float max_distance_sq,
                                       const int island_id,
                                       const bke::pbvh::GridsNode &node,
                                       NearestVertData &nvtd)
{
  for (const int grid : node.grids()) {
    const IndexRange grid_range = bke::ccg::grid_range(key, grid);
    BKE_subdiv_ccg_foreach_visible_grid_vert(key, grid_hidden, grid, [&](const int offset) {
      const int vert = grid_range[offset];
      if (ss.fake_neighbors.fake_neighbor_index[vert] != FAKE_NEIGHBOR_NONE) {
        return;
      }
      if (islands::vert_id_get(ss, vert) == island_id) {
        return;
      }
      const float distance_sq = math::distance_squared(positions[vert], location);
      if (distance_sq < max_distance_sq && distance_sq < nvtd.distance_sq) {
        nvtd.vert = vert;
        nvtd.distance_sq = distance_sq;
      }
    });
  }
}

static void fake_neighbor_search_bmesh(const SculptSession &ss,
                                       const float3 &location,
                                       const float max_distance_sq,
                                       const int island_id,
                                       const bke::pbvh::BMeshNode &node,
                                       NearestVertData &nvtd)
{
  for (const BMVert *bm_vert :
       BKE_pbvh_bmesh_node_unique_verts(const_cast<bke::pbvh::BMeshNode *>(&node)))
  {
    if (BM_elem_flag_test(bm_vert, BM_ELEM_HIDDEN)) {
      continue;
    }
    const int vert = BM_elem_index_get(bm_vert);
    if (ss.fake_neighbors.fake_neighbor_index[vert] != FAKE_NEIGHBOR_NONE) {
      continue;
    }
    if (islands::vert_id_get(ss, vert) == island_id) {
      continue;
    }
    const float distance_sq = math::distance_squared(float3(bm_vert->co), location);
    if (distance_sq < max_distance_sq && distance_sq < nvtd.distance_sq) {
      nvtd.vert = vert;
      nvtd.distance_sq = distance_sq;
    }
  }
}

static void fake_neighbor_search(const Depsgraph &depsgraph,
                                 const Object &ob,
                                 const float max_distance_sq,
                                 MutableSpan<int> fake_neighbors)
{
  PRF_scope(ProfileCategory::Editor);
  /* NOTE: This algorithm is extremely slow, it has O(n^2) runtime for the entire mesh. This looks
   * like the "closest pair of points" problem which should have far better solutions. */
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan<bool> hide_vert = *attributes.lookup<bool>(".hide_vert",
                                                                  bke::AttrDomain::Point);
      for (const int vert : vert_positions.index_range()) {
        if (fake_neighbors[vert] != FAKE_NEIGHBOR_NONE) {
          continue;
        }
        const int island_id = islands::vert_id_get(ss, vert);
        const float3 &location = vert_positions[vert];

        IndexMaskMemory memory;
        const IndexMask nodes_in_sphere = bke::pbvh::search_nodes(
            pbvh, memory, [&](const bke::pbvh::Node &node) {
              return node_in_sphere(node, location, max_distance_sq, false);
            });
        if (nodes_in_sphere.is_empty()) {
          continue;
        }
        const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const NearestVertData nvtd = threading::parallel_reduce(
            nodes_in_sphere.index_range(),
            1,
            NearestVertData(),
            [&](const IndexRange range, NearestVertData nvtd) {
              nodes_in_sphere.slice(range).foreach_index([&](const int i) {
                fake_neighbor_search_mesh(ss,
                                          vert_positions,
                                          hide_vert,
                                          location,
                                          max_distance_sq,
                                          island_id,
                                          nodes[i],
                                          nvtd);
              });
              return nvtd;
            },
            NearestVertData::join);
        if (nvtd.vert == -1) {
          continue;
        }
        fake_neighbors[vert] = nvtd.vert;
        fake_neighbors[nvtd.vert] = vert;
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const Span<float3> positions = subdiv_ccg.positions;
      const BitGroupVector<> grid_hidden = subdiv_ccg.grid_hidden;
      for (const int vert : positions.index_range()) {
        if (fake_neighbors[vert] != FAKE_NEIGHBOR_NONE) {
          continue;
        }
        const int island_id = islands::vert_id_get(ss, vert);
        const float3 &location = positions[vert];
        IndexMaskMemory memory;
        const IndexMask nodes_in_sphere = bke::pbvh::search_nodes(
            pbvh, memory, [&](const bke::pbvh::Node &node) {
              return node_in_sphere(node, location, max_distance_sq, false);
            });
        if (nodes_in_sphere.is_empty()) {
          continue;
        }
        const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        const NearestVertData nvtd = threading::parallel_reduce(
            nodes_in_sphere.index_range(),
            1,
            NearestVertData(),
            [&](const IndexRange range, NearestVertData nvtd) {
              nodes_in_sphere.slice(range).foreach_index([&](const int i) {
                fake_neighbor_search_grids(ss,
                                           key,
                                           positions,
                                           grid_hidden,
                                           location,
                                           max_distance_sq,
                                           island_id,
                                           nodes[i],
                                           nvtd);
              });
              return nvtd;
            },
            NearestVertData::join);
        if (nvtd.vert == -1) {
          continue;
        }
        fake_neighbors[vert] = nvtd.vert;
        fake_neighbors[nvtd.vert] = vert;
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const BMesh &bm = *ss.bm;
      for (const int vert : IndexRange(bm.totvert)) {
        if (fake_neighbors[vert] != FAKE_NEIGHBOR_NONE) {
          continue;
        }
        const int island_id = islands::vert_id_get(ss, vert);
        const float3 location = BM_vert_at_index(&const_cast<BMesh &>(bm), vert)->co;
        IndexMaskMemory memory;
        const IndexMask nodes_in_sphere = bke::pbvh::search_nodes(
            pbvh, memory, [&](const bke::pbvh::Node &node) {
              return node_in_sphere(node, location, max_distance_sq, false);
            });
        if (nodes_in_sphere.is_empty()) {
          continue;
        }
        const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
        const NearestVertData nvtd = threading::parallel_reduce(
            nodes_in_sphere.index_range(),
            1,
            NearestVertData(),
            [&](const IndexRange range, NearestVertData nvtd) {
              nodes_in_sphere.slice(range).foreach_index([&](const int i) {
                fake_neighbor_search_bmesh(
                    ss, location, max_distance_sq, island_id, nodes[i], nvtd);
              });
              return nvtd;
            },
            NearestVertData::join);
        if (nvtd.vert == -1) {
          continue;
        }
        fake_neighbors[vert] = nvtd.vert;
        fake_neighbors[nvtd.vert] = vert;
      }
      break;
    }
  }
}

Span<int> fake_neighbors_ensure(const Depsgraph &depsgraph, Object &ob, const float max_dist)
{
  SculptSession &ss = *ob.runtime->sculpt_session;

  /* Fake neighbors were already initialized with the same distance, so no need to be
   * recalculated. */
  if (!ss.fake_neighbors.fake_neighbor_index.is_empty() &&
      ss.fake_neighbors.current_max_distance == max_dist)
  {
    return ss.fake_neighbors.fake_neighbor_index;
  }

  islands::ensure_cache(ob);
  fake_neighbor_init(ob, max_dist);
  fake_neighbor_search(depsgraph, ob, max_dist * max_dist, ss.fake_neighbors.fake_neighbor_index);

  return ss.fake_neighbors.fake_neighbor_index;
}

void fake_neighbors_free(Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  pose_fake_neighbors_free(ss);
}

bool vertex_is_occluded(const Depsgraph &depsgraph,
                        const Object &object,
                        const float3 &position,
                        bool original)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  ViewContext *vc = ss.cache ? ss.cache->vc : &ss.filter_cache->vc;

  const float2 mouse = ED_view3d_project_float_v2_m4(
      vc->region, position, ss.cache ? ss.cache->projection_mat : ss.filter_cache->viewmat);

  float3 ray_start;
  float3 ray_end;
  float3 ray_normal;
  float depth = raycast_init(vc, mouse, ray_end, ray_start, ray_normal, original);

  ray_normal = ray_normal * -1.0f;
  ray_start = position + ray_normal * 0.002f;

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(const_cast<Object &>(object));

  RaycastData srd = {nullptr};
  srd.use_original = original;
  srd.object = &const_cast<Object &>(object);
  srd.is_mid_stroke = ss.cache != nullptr;
  srd.hit = false;
  srd.ray_start = ray_start;
  srd.ray_normal = ray_normal;
  srd.depth = depth;
  if (pbvh.type() == bke::pbvh::Type::Mesh) {
    const Mesh &mesh = *id_cast<const Mesh *>(object.data);
    srd.vert_positions = bke::pbvh::vert_positions_eval(depsgraph, object);
    srd.faces = mesh.faces();
    srd.corner_verts = mesh.corner_verts();
    srd.corner_tris = mesh.corner_tris();
  }
  else if (pbvh.type() == bke::pbvh::Type::Grids) {
    srd.subdiv_ccg = ss.subdiv_ccg;
  }
  vert_random_access_ensure(const_cast<Object &>(object));

  isect_ray_tri_watertight_v3_precalc(&srd.isect_precalc, ray_normal);
  bke::pbvh::raycast(
      pbvh,
      [&](bke::pbvh::Node &node, float *tmin) { sculpt_raycast_cb(node, srd, tmin); },
      ray_start,
      ray_normal,
      srd.use_original);

  return srd.hit;
}

namespace islands {

int vert_id_get(const SculptSession &ss, const int vert)
{
  BLI_assert(ss.topology_island_cache);
  if (!ss.topology_island_cache) {
    /* The cache should be calculated whenever it's necessary.
     * Still avoid crashing in release builds though. */
    return 0;
  }
  const SculptTopologyIslandCache &cache = *ss.topology_island_cache;
  if (!cache.vert_island_ids.is_empty()) {
    return cache.vert_island_ids[vert];
  }
  return 0;
}

void invalidate(SculptSession &ss)
{
  ss.topology_island_cache.reset();
}

static SculptTopologyIslandCache vert_disjoint_set_to_islands(const AtomicDisjointSet &vert_sets,
                                                              const int verts_num)
{
  Array<int> island_indices(verts_num);
  const int islands_num = vert_sets.calc_reduced_ids(island_indices);
  if (islands_num == 1) {
    return {};
  }

  Array<uint8_t> island_ids(island_indices.size());
  threading::parallel_for(island_ids.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      island_ids[i] = uint8_t(island_indices[i]);
    }
  });

  SculptTopologyIslandCache cache;
  cache.vert_island_ids = std::move(island_ids);
  return cache;
}

static SculptTopologyIslandCache calc_topology_islands_mesh(const Mesh &mesh)
{
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  IndexMaskMemory memory;
  const IndexMask visible_faces = hide_poly.is_empty() ?
                                      IndexMask(faces.size()) :
                                      IndexMask::from_bools_inverse(
                                          faces.index_range(), hide_poly, memory);

  AtomicDisjointSet disjoint_set(mesh.verts_num);
  visible_faces.foreach_index(
      [&](const int face) {
        const Span<int> face_verts = corner_verts.slice(faces[face]);
        for (const int i : face_verts.index_range().drop_front(1)) {
          disjoint_set.join(face_verts.first(), face_verts[i]);
        }
      },
      exec_mode::grain_size(1024));
  return vert_disjoint_set_to_islands(disjoint_set, mesh.verts_num);
}

/**
 * \todo Take grid face visibility into account.
 */
static SculptTopologyIslandCache calc_topology_islands_grids(const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  AtomicDisjointSet disjoint_set(subdiv_ccg.positions.size());
  threading::parallel_for(IndexRange(subdiv_ccg.grids_num), 512, [&](const IndexRange range) {
    for (const int grid : range) {
      SubdivCCGNeighbors neighbors;
      for (const short y : IndexRange(key.grid_size)) {
        for (const short x : IndexRange(key.grid_size)) {
          const SubdivCCGCoord coord{grid, x, y};
          SubdivCCGNeighbors neighbors;
          BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, true, neighbors);
          for (const SubdivCCGCoord neighbor : neighbors.coords) {
            disjoint_set.join(coord.to_index(key), neighbor.to_index(key));
          }
        }
      }
    }
  });

  return vert_disjoint_set_to_islands(disjoint_set, subdiv_ccg.positions.size());
}

static SculptTopologyIslandCache calc_topology_islands_bmesh(const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  BMesh &bm = *ss.bm;
  vert_random_access_ensure(const_cast<Object &>(object));

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
  AtomicDisjointSet disjoint_set(bm.totvert);
  node_mask.foreach_index(
      [&](const int i) {
        for (const BMFace *face :
             BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
        {
          if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
            continue;
          }
          disjoint_set.join(BM_elem_index_get(face->l_first->v),
                            BM_elem_index_get(face->l_first->next->v));
          disjoint_set.join(BM_elem_index_get(face->l_first->v),
                            BM_elem_index_get(face->l_first->next->next->v));
        }
      },
      exec_mode::grain_size(1));

  return vert_disjoint_set_to_islands(disjoint_set, bm.totvert);
}

static SculptTopologyIslandCache calculate_cache(const Object &object)
{
  PRF_scope(ProfileCategory::Editor);
  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh:
      return calc_topology_islands_mesh(*id_cast<const Mesh *>(object.data));
    case bke::pbvh::Type::Grids:
      return calc_topology_islands_grids(object);
    case bke::pbvh::Type::BMesh:
      return calc_topology_islands_bmesh(object);
  }
  BLI_assert_unreachable();
  return {};
}

void ensure_cache(Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  if (ss.topology_island_cache) {
    return;
  }
  ss.topology_island_cache = std::make_unique<SculptTopologyIslandCache>(calculate_cache(object));
}

}  // namespace islands

void cube_tip_init(const Sculpt & /*sd*/, const Object &ob, const Brush &brush, float mat[4][4])
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  float scale[4][4];
  float tmat[4][4];
  float unused[4][4];

  zero_m4(mat);
  calc_brush_local_mat(0.0, ob, unused, mat);

  /* NOTE: we ignore the radius scaling done inside of calc_brush_local_mat to
   * duplicate prior behavior.
   *
   * TODO: try disabling this and check that all edge cases work properly.
   */
  normalize_m4(mat);

  scale_m4_fl(scale, ss.cache->radius);
  mul_m4_m4m4(tmat, mat, scale);
  mul_v3_fl(tmat[1], brush.tip_scale_x);
  invert_m4_m4(mat, tmat);
}
/** \} */

MeshAttributeData::MeshAttributeData(const Mesh &mesh)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  this->mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
  this->hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
  this->hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  this->face_sets = *attributes.lookup<int>(".sculpt_face_set", bke::AttrDomain::Face);
}

void gather_bmesh_positions(const Set<BMVert *, 0> &verts, const MutableSpan<float3> positions)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == positions.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    positions[i] = vert->co;
    i++;
  }
}

void gather_grids_normals(const SubdivCCG &subdiv_ccg,
                          const Span<int> grids,
                          const MutableSpan<float3> normals)
{
  PRF_scope(ProfileCategory::Editor);
  gather_data_grids(subdiv_ccg, subdiv_ccg.normals.as_span(), grids, normals);
}

void gather_bmesh_normals(const Set<BMVert *, 0> &verts, const MutableSpan<float3> normals)
{
  PRF_scope(ProfileCategory::Editor);
  int i = 0;
  for (const BMVert *vert : verts) {
    normals[i] = vert->no;
    i++;
  }
}

template<typename T>
void gather_data_grids(const SubdivCCG &subdiv_ccg,
                       const Span<T> src,
                       const Span<int> grids,
                       const MutableSpan<T> node_data)
{
  PRF_scope(ProfileCategory::Editor);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  BLI_assert(grids.size() * key.grid_area == node_data.size());

  for (const int i : grids.index_range()) {
    const IndexRange grids_range = bke::ccg::grid_range(key, grids[i]);
    const IndexRange node_range = bke::ccg::grid_range(key, i);
    node_data.slice(node_range).copy_from(src.slice(grids_range));
  }
}

template<typename T>
void gather_data_bmesh(const Span<T> src,
                       const Set<BMVert *, 0> &verts,
                       const MutableSpan<T> node_data)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == node_data.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    node_data[i] = src[BM_elem_index_get(vert)];
    i++;
  }
}

template<typename T>
void scatter_data_grids(const SubdivCCG &subdiv_ccg,
                        const Span<T> node_data,
                        const Span<int> grids,
                        const MutableSpan<T> dst)
{
  PRF_scope(ProfileCategory::Editor);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  BLI_assert(grids.size() * key.grid_area == node_data.size());

  for (const int i : grids.index_range()) {
    const IndexRange grids_range = bke::ccg::grid_range(key, grids[i]);
    const IndexRange node_range = bke::ccg::grid_range(key, i);
    dst.slice(grids_range).copy_from(node_data.slice(node_range));
  }
}

template<typename T>
void scatter_data_bmesh(const Span<T> node_data,
                        const Set<BMVert *, 0> &verts,
                        const MutableSpan<T> dst)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == node_data.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    dst[BM_elem_index_get(vert)] = node_data[i];
    i++;
  }
}

template void gather_data_grids<int>(const SubdivCCG &, Span<int>, Span<int>, MutableSpan<int>);
template void gather_data_grids<float>(const SubdivCCG &,
                                       Span<float>,
                                       Span<int>,
                                       MutableSpan<float>);
template void gather_data_grids<float3>(const SubdivCCG &,
                                        Span<float3>,
                                        Span<int>,
                                        MutableSpan<float3>);
template void gather_data_bmesh<int>(Span<int>, const Set<BMVert *, 0> &, MutableSpan<int>);
template void gather_data_bmesh<float>(Span<float>, const Set<BMVert *, 0> &, MutableSpan<float>);
template void gather_data_bmesh<float3>(Span<float3>,
                                        const Set<BMVert *, 0> &,
                                        MutableSpan<float3>);

template void scatter_data_grids<float>(const SubdivCCG &,
                                        Span<float>,
                                        Span<int>,
                                        MutableSpan<float>);
template void scatter_data_grids<float3>(const SubdivCCG &,
                                         Span<float3>,
                                         Span<int>,
                                         MutableSpan<float3>);
template void scatter_data_bmesh<float>(Span<float>, const Set<BMVert *, 0> &, MutableSpan<float>);
template void scatter_data_bmesh<float3>(Span<float3>,
                                         const Set<BMVert *, 0> &,
                                         MutableSpan<float3>);

void calc_factors_common_mesh_indexed(const Depsgraph &depsgraph,
                                      const Brush &brush,
                                      const Object &object,
                                      const MeshAttributeData &attribute_data,
                                      const Span<float3> vert_positions,
                                      const Span<float3> vert_normals,
                                      const bke::pbvh::MeshNode &node,
                                      Vector<float> &r_factors,
                                      Vector<float> &r_distances)
{
  const Span<int> verts = node.verts();
  r_factors.resize(verts.size());
  r_distances.resize(verts.size());

  calc_factors_common_mesh_indexed(depsgraph,
                                   brush,
                                   object,
                                   attribute_data,
                                   vert_positions,
                                   vert_normals,
                                   node,
                                   r_factors.as_mutable_span(),
                                   r_distances.as_mutable_span());
}
void calc_factors_common_mesh_indexed(const Depsgraph &depsgraph,
                                      const Brush &brush,
                                      const Object &object,
                                      const MeshAttributeData &attribute_data,
                                      const Span<float3> vert_positions,
                                      const Span<float3> vert_normals,
                                      const bke::pbvh::MeshNode &node,
                                      const MutableSpan<float> factors,
                                      const MutableSpan<float> distances)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, vert_positions, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  calc_brush_distances(
      ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, vert_positions, verts, factors);
}

void calc_factors_common_mesh(const Depsgraph &depsgraph,
                              const Brush &brush,
                              const Object &object,
                              const MeshAttributeData &attribute_data,
                              const Span<float3> positions,
                              const Span<float3> vert_normals,
                              const bke::pbvh::MeshNode &node,
                              Vector<float> &r_factors,
                              Vector<float> &r_distances)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  r_factors.resize(verts.size());
  const MutableSpan<float> factors = r_factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  r_distances.resize(verts.size());
  const MutableSpan<float> distances = r_distances;
  calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);
}

void calc_factors_common_grids(const Depsgraph &depsgraph,
                               const Brush &brush,
                               const Object &object,
                               const Span<float3> positions,
                               const bke::pbvh::GridsNode &node,
                               Vector<float> &r_factors,
                               Vector<float> &r_distances)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();

  r_factors.resize(positions.size());
  const MutableSpan<float> factors = r_factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, subdiv_ccg, grids, factors);
  }

  r_distances.resize(positions.size());
  const MutableSpan<float> distances = r_distances;
  calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  calc_brush_texture_factors(ss, brush, subdiv_ccg, grids, positions, factors);
}

void calc_factors_common_bmesh(const Depsgraph &depsgraph,
                               const Brush &brush,
                               const Object &object,
                               const Span<float3> positions,
                               bke::pbvh::BMeshNode &node,
                               Vector<float> &r_factors,
                               Vector<float> &r_distances)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  r_factors.resize(verts.size());
  const MutableSpan<float> factors = r_factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, verts, factors);
  }

  r_distances.resize(verts.size());
  const MutableSpan<float> distances = r_distances;
  calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);
}

void calc_factors_common_from_orig_data_mesh(const Depsgraph &depsgraph,
                                             const Brush &brush,
                                             const Object &object,
                                             const MeshAttributeData &attribute_data,
                                             const Span<float3> positions,
                                             const Span<float3> normals,
                                             const bke::pbvh::MeshNode &node,
                                             Vector<float> &r_factors,
                                             Vector<float> &r_distances)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  r_factors.resize(verts.size());
  const MutableSpan<float> factors = r_factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, positions, factors);

  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, normals, factors);
  }

  r_distances.resize(verts.size());
  const MutableSpan<float> distances = r_distances;
  calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);
}

void calc_factors_common_from_orig_data_grids(const Depsgraph &depsgraph,
                                              const Brush &brush,
                                              const Object &object,
                                              const Span<float3> positions,
                                              const Span<float3> normals,
                                              const bke::pbvh::GridsNode &node,
                                              Vector<float> &r_factors,
                                              Vector<float> &r_distances)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();

  r_factors.resize(positions.size());
  const MutableSpan<float> factors = r_factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, normals, factors);
  }

  r_distances.resize(positions.size());
  const MutableSpan<float> distances = r_distances;
  calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  calc_brush_texture_factors(ss, brush, subdiv_ccg, grids, positions, factors);
}

void calc_factors_common_from_orig_data_bmesh(const Depsgraph &depsgraph,
                                              const Brush &brush,
                                              const Object &object,
                                              const Span<float3> positions,
                                              const Span<float3> normals,
                                              bke::pbvh::BMeshNode &node,
                                              Vector<float> &r_factors,
                                              Vector<float> &r_distances)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  r_factors.resize(verts.size());
  const MutableSpan<float> factors = r_factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, normals, factors);
  }

  r_distances.resize(verts.size());
  const MutableSpan<float> distances = r_distances;
  calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);
}

void fill_factor_from_hide(const Span<bool> hide_vert,
                           const Span<int> verts,
                           const MutableSpan<float> r_factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == r_factors.size());

  if (!hide_vert.is_empty()) {
    for (const int i : verts.index_range()) {
      r_factors[i] = hide_vert[verts[i]] ? 0.0f : 1.0f;
    }
  }
  else {
    r_factors.fill(1.0f);
  }
}

void fill_factor_from_hide(const SubdivCCG &subdiv_ccg,
                           const Span<int> grids,
                           const MutableSpan<float> r_factors)
{
  PRF_scope(ProfileCategory::Editor);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  BLI_assert(grids.size() * key.grid_area == r_factors.size());

  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  if (grid_hidden.is_empty()) {
    r_factors.fill(1.0f);
    return;
  }
  for (const int i : grids.index_range()) {
    const BitSpan hidden = grid_hidden[grids[i]];
    const int start = i * key.grid_area;
    for (const int offset : IndexRange(key.grid_area)) {
      r_factors[start + offset] = hidden[offset] ? 0.0f : 1.0f;
    }
  }
}

void fill_factor_from_hide(const Set<BMVert *, 0> &verts, const MutableSpan<float> r_factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == r_factors.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    r_factors[i] = BM_elem_flag_test_bool(vert, BM_ELEM_HIDDEN) ? 0.0f : 1.0f;
    i++;
  }
}

void fill_factor_from_hide_and_mask(const Span<bool> hide_vert,
                                    const Span<float> mask,
                                    const Span<int> verts,
                                    const MutableSpan<float> r_factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == r_factors.size());

  if (!mask.is_empty()) {
    for (const int i : verts.index_range()) {
      r_factors[i] = 1.0f - mask[verts[i]];
    }
  }
  else {
    r_factors.fill(1.0f);
  }

  if (!hide_vert.is_empty()) {
    for (const int i : verts.index_range()) {
      if (hide_vert[verts[i]]) {
        r_factors[i] = 0.0f;
      }
    }
  }
}

void fill_factor_from_hide_and_mask(const BMesh &bm,
                                    const Set<BMVert *, 0> &verts,
                                    const MutableSpan<float> r_factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == r_factors.size());

  /* TODO: Avoid overhead of accessing attributes for every bke::pbvh::Tree node. */
  const int mask_offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
  int i = 0;
  for (const BMVert *vert : verts) {
    r_factors[i] = (mask_offset == -1) ? 1.0f : 1.0f - BM_ELEM_CD_GET_FLOAT(vert, mask_offset);
    if (BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
      r_factors[i] = 0.0f;
    }
    i++;
  }
}

void fill_factor_from_hide_and_mask(const SubdivCCG &subdiv_ccg,
                                    const Span<int> grids,
                                    const MutableSpan<float> r_factors)
{
  PRF_scope(ProfileCategory::Editor);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  BLI_assert(grids.size() * key.grid_area == r_factors.size());

  if (!subdiv_ccg.masks.is_empty()) {
    const Span<float> masks = subdiv_ccg.masks;
    for (const int i : grids.index_range()) {
      const Span src = masks.slice(bke::ccg::grid_range(key, grids[i]));
      MutableSpan dst = r_factors.slice(bke::ccg::grid_range(key, i));
      for (const int offset : dst.index_range()) {
        dst[offset] = 1.0f - src[offset];
      }
    }
  }
  else {
    r_factors.fill(1.0f);
  }

  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  if (!grid_hidden.is_empty()) {
    for (const int i : grids.index_range()) {
      const BitSpan hidden = grid_hidden[grids[i]];
      const int start = i * key.grid_area;
      for (const int offset : IndexRange(key.grid_area)) {
        if (hidden[offset]) {
          r_factors[start + offset] = 0.0f;
        }
      }
    }
  }
}

void calc_front_face(const float3 &view_normal,
                     const Span<float3> vert_normals,
                     const Span<int> verts,
                     const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == factors.size());

  for (const int i : verts.index_range()) {
    const float dot = math::dot(view_normal, vert_normals[verts[i]]);
    factors[i] *= std::max(dot, 0.0f);
  }
}

void calc_front_face(const float3 &view_normal,
                     const Span<float3> normals,
                     const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(normals.size() == factors.size());

  for (const int i : normals.index_range()) {
    const float dot = math::dot(view_normal, normals[i]);
    factors[i] *= std::max(dot, 0.0f);
  }
}
void calc_front_face(const float3 &view_normal,
                     const SubdivCCG &subdiv_ccg,
                     const Span<int> grids,
                     const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float3> normals = subdiv_ccg.normals;
  BLI_assert(grids.size() * key.grid_area == factors.size());

  for (const int i : grids.index_range()) {
    const Span<float3> grid_normals = normals.slice(bke::ccg::grid_range(key, grids[i]));
    MutableSpan<float> grid_factors = factors.slice(bke::ccg::grid_range(key, i));
    for (const int offset : grid_factors.index_range()) {
      const float dot = math::dot(view_normal, grid_normals[offset]);
      grid_factors[offset] *= std::max(dot, 0.0f);
    }
  }
}

void calc_front_face(const float3 &view_normal,
                     const Set<BMVert *, 0> &verts,
                     const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == factors.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    const float dot = math::dot(view_normal, float3(vert->no));
    factors[i] *= std::max(dot, 0.0f);
    i++;
  }
}

void calc_front_face(const float3 &view_normal,
                     const Set<BMFace *, 0> &faces,
                     const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(faces.size() == factors.size());

  int i = 0;
  for (const BMFace *face : faces) {
    const float dot = math::dot(view_normal, float3(face->no));
    factors[i] *= std::max(dot, 0.0f);
    i++;
  }
}

void filter_region_clip_factors(const SculptSession &ss,
                                const Span<float3> positions,
                                const Span<int> verts,
                                const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == factors.size());

  const RegionView3D *rv3d = ss.cache ? ss.cache->vc->rv3d : ss.rv3d;
  const View3D *v3d = ss.cache ? ss.cache->vc->v3d : ss.v3d;
  if (!RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    return;
  }

  const ePaintSymmetryFlags mirror_symmetry_pass = ss.cache ? ss.cache->mirror_symmetry_pass :
                                                              ePaintSymmetryFlags(0);
  const int radial_symmetry_pass = ss.cache ? ss.cache->radial_symmetry_pass : 0;
  const float4x4 symm_rot_mat_inv = ss.cache ? ss.cache->symm_rot_mat_inv : float4x4::identity();
  for (const int i : verts.index_range()) {
    float3 symm_co = symmetry_flip(positions[verts[i]], mirror_symmetry_pass);
    if (radial_symmetry_pass) {
      symm_co = math::transform_point(symm_rot_mat_inv, symm_co);
    }
    if (ED_view3d_clipping_test(rv3d, symm_co, true)) {
      factors[i] = 0.0f;
    }
  }
}

void filter_region_clip_factors(const SculptSession &ss,
                                const Span<float3> positions,
                                const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(positions.size() == factors.size());

  const RegionView3D *rv3d = ss.cache ? ss.cache->vc->rv3d : ss.rv3d;
  const View3D *v3d = ss.cache ? ss.cache->vc->v3d : ss.v3d;
  if (!RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    return;
  }

  const ePaintSymmetryFlags mirror_symmetry_pass = ss.cache ? ss.cache->mirror_symmetry_pass :
                                                              ePaintSymmetryFlags(0);
  const int radial_symmetry_pass = ss.cache ? ss.cache->radial_symmetry_pass : 0;
  const float4x4 symm_rot_mat_inv = ss.cache ? ss.cache->symm_rot_mat_inv : float4x4::identity();
  for (const int i : positions.index_range()) {
    float3 symm_co = symmetry_flip(positions[i], mirror_symmetry_pass);
    if (radial_symmetry_pass) {
      symm_co = math::transform_point(symm_rot_mat_inv, symm_co);
    }
    if (ED_view3d_clipping_test(rv3d, symm_co, true)) {
      factors[i] = 0.0f;
    }
  }
}

void calc_brush_distances_squared(const SculptSession &ss,
                                  const Span<float3> positions,
                                  const Span<int> verts,
                                  const eBrushFalloffShape falloff_shape,
                                  const MutableSpan<float> r_distances)
{
  BLI_assert(verts.size() == r_distances.size());

  const float3 &test_location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE && (ss.cache || ss.filter_cache)) {
    /* The tube falloff shape requires the cached view normal. */
    const float3 &view_normal = ss.cache ? ss.cache->view_normal_symm :
                                           ss.filter_cache->view_normal;
    float4 test_plane;
    plane_from_point_normal_v3(test_plane, test_location, view_normal);
    for (const int i : verts.index_range()) {
      float3 projected;
      closest_to_plane_normalized_v3(projected, test_plane, positions[verts[i]]);
      r_distances[i] = math::distance_squared(projected, test_location);
    }
  }
  else {
    for (const int i : verts.index_range()) {
      r_distances[i] = math::distance_squared(test_location, positions[verts[i]]);
    }
  }
}

void calc_brush_distances(const SculptSession &ss,
                          const Span<float3> positions,
                          const Span<int> verts,
                          const eBrushFalloffShape falloff_shape,
                          const MutableSpan<float> r_distances)
{
  PRF_scope(ProfileCategory::Editor);
  calc_brush_distances_squared(ss, positions, verts, falloff_shape, r_distances);
  for (float &value : r_distances) {
    value = std::sqrt(value);
  }
}

void calc_brush_distances_squared(const SculptSession &ss,
                                  const Span<float3> positions,
                                  const eBrushFalloffShape falloff_shape,
                                  const MutableSpan<float> r_distances)
{
  BLI_assert(positions.size() == r_distances.size());

  const float3 &test_location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE && (ss.cache || ss.filter_cache)) {
    /* The tube falloff shape requires the cached view normal. */
    const float3 &view_normal = ss.cache ? ss.cache->view_normal_symm :
                                           ss.filter_cache->view_normal;
    float4 test_plane;
    plane_from_point_normal_v3(test_plane, test_location, view_normal);
    for (const int i : positions.index_range()) {
      float3 projected;
      closest_to_plane_normalized_v3(projected, test_plane, positions[i]);
      r_distances[i] = math::distance_squared(projected, test_location);
    }
  }
  else {
    for (const int i : positions.index_range()) {
      r_distances[i] = math::distance_squared(test_location, positions[i]);
    }
  }
}

void calc_brush_distances(const SculptSession &ss,
                          const Span<float3> positions,
                          const eBrushFalloffShape falloff_shape,
                          const MutableSpan<float> r_distances)
{
  PRF_scope(ProfileCategory::Editor);
  calc_brush_distances_squared(ss, positions, falloff_shape, r_distances);
  for (float &value : r_distances) {
    value = std::sqrt(value);
  }
}

void filter_distances_with_radius(const float radius,
                                  const Span<float> distances,
                                  const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : distances.index_range()) {
    if (distances[i] >= radius) {
      factors[i] = 0.0f;
    }
  }
}

template<typename T>
void calc_brush_cube_distances(const Brush &brush,
                               const Span<T> positions,
                               const MutableSpan<float> r_distances)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(r_distances.size() == positions.size());

  const float roundness = brush.tip_roundness;
  const float roundness_rcp = math::safe_rcp(roundness);
  const float hardness = 1.0f - roundness;

  for (const int i : positions.index_range()) {
    const T local = math::abs(positions[i]);

    if (math::reduce_max(local) > 1.0f) {
      r_distances[i] = 1.0f;
      continue;
    }
    if (std::min(local.x, local.y) > hardness) {
      /* Corner, distance to the center of the corner circle. */
      r_distances[i] = math::distance(float2(hardness), float2(local)) * roundness_rcp;
      continue;
    }
    if (std::max(local.x, local.y) > hardness) {
      /* Side, distance to the square XY axis. */
      r_distances[i] = (std::max(local.x, local.y) - hardness) * roundness_rcp;
      continue;
    }

    /* Inside the square, constant distance. */
    r_distances[i] = 0.0f;
  }
}
template void calc_brush_cube_distances<float2>(const Brush &brush,
                                                const Span<float2> positions,
                                                MutableSpan<float> r_distances);
template void calc_brush_cube_distances<float3>(const Brush &brush,
                                                const Span<float3> positions,
                                                MutableSpan<float> r_distances);

void apply_hardness_to_distances(const float radius,
                                 const float hardness,
                                 const MutableSpan<float> distances)
{
  PRF_scope(ProfileCategory::Editor);
  if (hardness == 0.0f) {
    return;
  }
  const float threshold = hardness * radius;
  if (hardness == 1.0f) {
    for (const int i : distances.index_range()) {
      distances[i] = distances[i] < threshold ? 0.0f : radius;
    }
    return;
  }
  const float radius_inv = math::rcp(radius);
  const float hardness_inv_rcp = math::rcp(1.0f - hardness);
  for (const int i : distances.index_range()) {
    if (distances[i] < threshold) {
      distances[i] = 0.0f;
    }
    else {
      const float radius_factor = (distances[i] * radius_inv - hardness) * hardness_inv_rcp;
      distances[i] = radius_factor * radius;
    }
  }
}

void calc_brush_strength_factors(const StrokeCache &cache,
                                 const Brush &brush,
                                 const Span<float> distances,
                                 const MutableSpan<float> factors)
{
  BKE_brush_calc_curve_factors(eBrushCurvePreset(brush.curve_distance_falloff_preset),
                               brush.curve_distance_falloff,
                               distances,
                               cache.radius,
                               factors);
}

void calc_brush_texture_factors(const SculptSession &ss,
                                const Brush &brush,
                                const Span<float3> vert_positions,
                                const Span<int> verts,
                                const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == factors.size());

  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (!mtex->tex) {
    return;
  }
  const int thread_id = BLI_task_parallel_thread_id(nullptr);

  for (const int i : verts.index_range()) {
    if (factors[i] == 0.0f) {
      continue;
    }
    float texture_value;
    float4 texture_rgba;
    /* NOTE: This is not a thread-safe call. */
    sculpt_apply_texture(
        ss, brush, vert_positions[verts[i]], thread_id, &texture_value, texture_rgba);

    factors[i] *= texture_value;
  }
}

void calc_brush_texture_factors(const SculptSession &ss,
                                const Brush &brush,
                                const Span<float3> positions,
                                const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(positions.size() == factors.size());

  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (!mtex->tex) {
    return;
  }
  const int thread_id = BLI_task_parallel_thread_id(nullptr);

  for (const int i : positions.index_range()) {
    if (factors[i] == 0.0f) {
      continue;
    }
    float texture_value;
    float4 texture_rgba;
    /* NOTE: This is not a thread-safe call. */
    sculpt_apply_texture(ss, brush, positions[i], thread_id, &texture_value, texture_rgba);

    factors[i] *= texture_value;
  }
}

/** ANCHORED_NORMAL_GUIDED_V2: direct per-grid CCG normal lookup; sparse sampling
 * keeps the normal-driven projection from allocating an extra node-sized array. */
void calc_brush_texture_factors(const SculptSession &ss,
                                const Brush &brush,
                                const SubdivCCG &subdiv_ccg,
                                const Span<int> grids,
                                const Span<float3> positions,
                                const MutableSpan<float> factors)
{
  if (brush.stroke_method != BRUSH_STROKE_ANCHORED ||
      !(brush.flag2 & BRUSH_SCULPT_ANCHOR_SURFACE_PROJECT) ||
      !(brush.flag2 & (BRUSH_SCULPT_ANCHOR_NORMAL_GUIDED | BRUSH_SCULPT_ANCHOR_GEODESIC)))
  {
    calc_brush_texture_factors(ss, brush, positions, factors);
    return;
  }

  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (!mtex->tex || mtex->brush_map_mode != MTEX_MAP_MODE_AREA) {
    calc_brush_texture_factors(ss, brush, positions, factors);
    return;
  }

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float3> normals = subdiv_ccg.normals.as_span();
  if (key.grid_area <= 0 || grids.size() * key.grid_area != positions.size() ||
      normals.size() < subdiv_ccg.grids_num * key.grid_area)
  {
    calc_brush_texture_factors(ss, brush, positions, factors);
    return;
  }

  const int thread_id = BLI_task_parallel_thread_id(nullptr);
  for (const int grid_i : grids.index_range()) {
    const int global_grid = grids[grid_i];
    if (global_grid < 0 || global_grid >= subdiv_ccg.grids_num) {
      continue;
    }
    const int local_offset = grid_i * key.grid_area;
    const int normal_offset = global_grid * key.grid_area;
    for (const int j : IndexRange(key.grid_area)) {
      const int local_i = local_offset + j;
      if (factors[local_i] == 0.0f) {
        continue;
      }
      float texture_value;
      float4 texture_rgba;
      const float3 &normal = normals[normal_offset + j];
      sculpt_apply_texture(ss,
                           brush,
                           positions[local_i],
                           thread_id,
                           &texture_value,
                           texture_rgba,
                           &normal,
                           normal_offset + j);
      factors[local_i] *= texture_value;
    }
  }
}

void reset_translations_to_original(const MutableSpan<float3> translations,
                                    const Span<float3> positions,
                                    const Span<float3> orig_positions)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(translations.size() == orig_positions.size());
  BLI_assert(translations.size() == positions.size());
  for (const int i : translations.index_range()) {
    const float3 prev_translation = positions[i] - orig_positions[i];
    translations[i] -= prev_translation;
  }
}

#ifndef NDEBUG
static bool contains_nan(const Span<float> values)
{
  return std::any_of(values.begin(), values.end(), [&](const float v) { return std::isnan(v); });
}
#endif

void apply_translations(const Span<float3> translations,
                        const Span<int> verts,
                        const MutableSpan<float3> positions)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == translations.size());
  BLI_assert(!contains_nan(translations.cast<float>()));

  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    positions[vert] += translations[i];
  }
}

void apply_translations(const Span<float3> translations,
                        const Span<int> grids,
                        SubdivCCG &subdiv_ccg)
{
  PRF_scope(ProfileCategory::Editor);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  MutableSpan<float3> positions = subdiv_ccg.positions;
  BLI_assert(grids.size() * key.grid_area == translations.size());
  BLI_assert(!contains_nan(translations.cast<float>()));

  /* SCULPT_EXACT_DIRTY_GRIDS_WRITER_V1
   *
   * PBVH dirty nodes are conservative. Record which CCG grids actually receive
   * a non-zero translation so Multires layer commit can avoid solving every
   * grid owned by the touched PBVH nodes. */
  Vector<int, 32> exact_dirty_grids;


  /* SCULPT_EXACT_DIRTY_SAMPLES_WRITER_V1_1
   * Collect sample IDs locally; merge under the same mutex used by
   * the exact-grid journal so parallel brush tasks remain safe. */
  Vector<int64_t, 256> exact_dirty_samples;
  for (const int i : grids.index_range()) {
    const Span<float3> grid_translations = translations.slice(bke::ccg::grid_range(key, i));
    MutableSpan<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grids[i]));
    bool grid_changed = false;

    for (const int offset : grid_positions.index_range()) {
      const float3 delta = grid_translations[offset];
      if (delta.x != 0.0f || delta.y != 0.0f || delta.z != 0.0f) {
        grid_positions[offset] += delta;
        grid_changed = true;
        exact_dirty_samples.append(int64_t(grids[i]) * int64_t(key.grid_area) + offset);
      }
    }

    if (grid_changed) {
      exact_dirty_grids.append(grids[i]);
    }
  }

  if (!exact_dirty_grids.is_empty()) {
    std::lock_guard<std::mutex> lock(subdiv_ccg.sculpt_dirty_position_grids_mutex);

    if (subdiv_ccg.sculpt_dirty_position_grid_flags.size() != subdiv_ccg.grids_num) {
      subdiv_ccg.sculpt_dirty_position_grid_flags.reinitialize(subdiv_ccg.grids_num);
      subdiv_ccg.sculpt_dirty_position_grid_flags.fill(false);
      subdiv_ccg.sculpt_dirty_position_grids.clear();
    }

    const int64_t position_samples = subdiv_ccg.positions.size();

    if (subdiv_ccg.sculpt_dirty_position_sample_flags.size() != position_samples) {

      subdiv_ccg.sculpt_dirty_position_sample_flags.resize(position_samples, false);

      subdiv_ccg.sculpt_dirty_position_sample_flags.fill(false);

    }

    for (const int64_t sample : exact_dirty_samples) {

      if (sample >= 0 && sample < position_samples) {

        subdiv_ccg.sculpt_dirty_position_sample_flags[sample].set();

      }

    }


    for (const int grid : exact_dirty_grids) {
      if (grid < 0 || grid >= subdiv_ccg.grids_num) {
        continue;
      }
      if (!subdiv_ccg.sculpt_dirty_position_grid_flags[grid]) {
        subdiv_ccg.sculpt_dirty_position_grid_flags[grid] = true;
        subdiv_ccg.sculpt_dirty_position_grids.append(grid);
      }
    }
  }
}

void apply_translations(const Span<float3> translations, const Set<BMVert *, 0> &verts)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == translations.size());
  BLI_assert(!contains_nan(translations.cast<float>()));

  int i = 0;
  for (BMVert *vert : verts) {
    add_v3_v3(vert->co, translations[i]);
    i++;
  }
}

void project_translations(const MutableSpan<float3> translations, const float3 &plane)
{
  PRF_scope(ProfileCategory::Editor);
  /* Equivalent to #project_plane_v3_v3v3. */
  const float len_sq = math::length_squared(plane);
  if (len_sq < std::numeric_limits<float>::epsilon()) {
    return;
  }
  const float dot_factor = -math::rcp(len_sq);
  for (const int i : translations.index_range()) {
    translations[i] += plane * math::dot(translations[i], plane) * dot_factor;
  }
}

void apply_crazyspace_to_translations(const Span<float3x3> deform_imats,
                                      const Span<int> verts,
                                      const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == translations.size());

  for (const int i : verts.index_range()) {
    translations[i] = math::transform_point(deform_imats[verts[i]], translations[i]);
  }
}

void clip_and_lock_translations(const Sculpt &sd,
                                const SculptSession &ss,
                                const Span<float3> positions,
                                const Span<int> verts,
                                const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == translations.size());

  const StrokeCache *cache = ss.cache;
  if (!cache) {
    return;
  }
  for (const int axis : IndexRange(3)) {
    if (sd.flags & (SCULPT_LOCK_X << axis)) {
      for (float3 &translation : translations) {
        translation[axis] = 0.0f;
      }
      continue;
    }

    if (!(cache->mirror_modifier_clip.flag & (uint8_t(StrokeFlags::ClipX) << axis))) {
      continue;
    }

    const float4x4 mirror(cache->mirror_modifier_clip.mat);
    const float4x4 mirror_inverse(cache->mirror_modifier_clip.mat_inv);
    for (const int i : verts.index_range()) {
      const int vert = verts[i];

      /* Transform into the space of the mirror plane, check translations, then transform back. */
      float3 co_mirror = math::transform_point(mirror, positions[vert]);
      if (math::abs(co_mirror[axis]) > cache->mirror_modifier_clip.tolerance[axis]) {
        continue;
      }
      /* Clear the translation in the local space of the mirror object. */
      co_mirror[axis] = 0.0f;
      const float3 co_local = math::transform_point(mirror_inverse, co_mirror);
      translations[i][axis] = co_local[axis] - positions[vert][axis];
    }
  }
}

void clip_and_lock_translations(const Sculpt &sd,
                                const SculptSession &ss,
                                const Span<float3> positions,
                                const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(positions.size() == translations.size());

  const StrokeCache *cache = ss.cache;
  if (!cache) {
    return;
  }
  for (const int axis : IndexRange(3)) {
    if (sd.flags & (SCULPT_LOCK_X << axis)) {
      for (float3 &translation : translations) {
        translation[axis] = 0.0f;
      }
      continue;
    }

    if (!(cache->mirror_modifier_clip.flag & (uint8_t(StrokeFlags::ClipX) << axis))) {
      continue;
    }

    const float4x4 mirror(cache->mirror_modifier_clip.mat);
    const float4x4 mirror_inverse(cache->mirror_modifier_clip.mat_inv);
    for (const int i : positions.index_range()) {
      /* Transform into the space of the mirror plane, check translations, then transform back. */
      float3 co_mirror = math::transform_point(mirror, positions[i]);
      if (math::abs(co_mirror[axis]) > cache->mirror_modifier_clip.tolerance[axis]) {
        continue;
      }
      /* Clear the translation in the local space of the mirror object. */
      co_mirror[axis] = 0.0f;
      const float3 co_local = math::transform_point(mirror_inverse, co_mirror);
      translations[i][axis] = co_local[axis] - positions[i][axis];
    }
  }
}

std::optional<ShapeKeyData> ShapeKeyData::from_object(Object &object)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  Key *keys = mesh.key;
  if (!keys) {
    return std::nullopt;
  }
  const int active_index = object.shapenr - 1;
  const KeyBlock *active_key = BKE_keyblock_find_by_index(keys, active_index);
  if (!active_key) {
    return std::nullopt;
  }
  ShapeKeyData data;
  data.active_key_data = {static_cast<float3 *>(active_key->data), active_key->totelem};
  data.basis_key_active = active_key == keys->refkey;
  if (const std::optional<Array<bool>> dependent = BKE_keyblock_get_dependent_keys(keys,
                                                                                   active_index))
  {

    for (const auto [i, other_key] : keys->block.enumerate()) {
      if ((&other_key != active_key) && (*dependent)[i]) {
        data.dependent_keys.append({static_cast<float3 *>(other_key.data), other_key.totelem});
      }
    }
  }
  return data;
}

/* Sculpt Mesh V3 Deep Probe: diagnostics only, enabled unless explicitly set to 0. */
static bool sculpt_mesh_v3deep_enabled()
{
  static const bool enabled = []() {
    const char *value = std::getenv("BLENDER_SCULPT_V3_DEEP_PROBE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

PositionDeformData::PositionDeformData(const Depsgraph &depsgraph, Object &object_orig)
{
  Mesh &mesh = *id_cast<Mesh *>(object_orig.data);
  const bool v3deep = sculpt_mesh_v3deep_enabled() && mesh.verts_num >= 1'000'000;
  const auto v3deep_mem_start = v3deep ? sculpt_mesh_v3_memory_sample() : SculptMeshV3MemorySample{};
  const void *v3deep_orig_before = v3deep ? mesh.vert_positions().data() : nullptr;
  const double v3deep_begin = v3deep ? BLI_time_now_seconds() : 0.0;
  this->eval = bke::pbvh::vert_positions_eval(depsgraph, object_orig);

  if (!object_orig.runtime->sculpt_session->deform_imats.is_empty()) {
    deform_imats_ = object_orig.runtime->sculpt_session->deform_imats;
  }
  base_layer_ = bke::multires_layers::base_recording_layer(object_orig);
  if (MultiresModifierData *base_mmd = bke::multires_layers::base_modifier(object_orig)) {
    base_recording_blocked_ =
        !(base_mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh) &&
        bke::multires_layers::base_has_visible_layers(*base_mmd) && !base_layer_;
  }
  if (base_layer_ && !bke::multires_layers::base_deltas_ensure(*base_layer_, mesh.verts_num)) {
    base_recording_blocked_ = true;
  }
  const auto v3deep_mem_before_orig = v3deep ? sculpt_mesh_v3_memory_sample() : SculptMeshV3MemorySample{};
  const double v3deep_orig_start = v3deep ? BLI_time_now_seconds() : 0.0;
  orig_ = mesh.vert_positions_for_write();
  const double v3deep_orig_end = v3deep ? BLI_time_now_seconds() : 0.0;
  const auto v3deep_mem_orig = v3deep ? sculpt_mesh_v3_memory_sample() : SculptMeshV3MemorySample{};

  const double v3deep_eval_start = v3deep ? BLI_time_now_seconds() : 0.0;
  MutableSpan eval_mut = bke::pbvh::vert_positions_eval_for_write(depsgraph, object_orig);
  /* Sculpt Mesh V3 Local Normals Fix: writable access can detach shared storage.
   * Keep brush reads and bounds updates on the current evaluated position buffer. */
  this->eval = eval_mut.as_span();
  const double v3deep_eval_end = v3deep ? BLI_time_now_seconds() : 0.0;
  const auto v3deep_mem_eval = v3deep ? sculpt_mesh_v3_memory_sample() : SculptMeshV3MemorySample{};
  if (eval_mut.data() != orig_.data()) {
    eval_mut_ = eval_mut;
  }

  shape_key_data_ = ShapeKeyData::from_object(object_orig);
  if (v3deep) {
    const double total_ms = (BLI_time_now_seconds() - v3deep_begin) * 1000.0;
    if (total_ms >= 0.50 || v3deep_orig_before != orig_.data()) {
      constexpr double mib = 1024.0 * 1024.0;
      const auto private_delta = [&](const SculptMeshV3MemorySample &a,
                                     const SculptMeshV3MemorySample &b) {
        return a.valid && b.valid ?
                   double(int64_t(b.private_bytes) - int64_t(a.private_bytes)) / mib : 0.0;
      };
      const auto faults = [&](const SculptMeshV3MemorySample &a,
                              const SculptMeshV3MemorySample &b) -> unsigned long long {
        return a.valid && b.valid && b.page_faults >= a.page_faults ?
                   static_cast<unsigned long long>(b.page_faults - a.page_faults) : 0;
      };
      std::fprintf(stderr,
                   "[SculptMeshV3Pos] verts=%d total=%.3f ms before_write=%.3f "
                   "orig_write=%.3f eval_write=%.3f before_private=%+.2f MiB orig_private=%+.2f MiB "
                   "eval_private=%+.2f MiB before_pf=%llu orig_pf=%llu eval_pf=%llu "
                   "orig_changed=%d read_matches_orig=%d read_matches_eval=%d\n",
                   mesh.verts_num, total_ms,
                   (v3deep_orig_start - v3deep_begin) * 1000.0,
                   (v3deep_orig_end - v3deep_orig_start) * 1000.0,
                   (v3deep_eval_end - v3deep_eval_start) * 1000.0,
                   private_delta(v3deep_mem_start, v3deep_mem_before_orig),
                   private_delta(v3deep_mem_before_orig, v3deep_mem_orig),
                   private_delta(v3deep_mem_orig, v3deep_mem_eval),
                   faults(v3deep_mem_start, v3deep_mem_before_orig),
                   faults(v3deep_mem_before_orig, v3deep_mem_orig),
                   faults(v3deep_mem_orig, v3deep_mem_eval),
                   int(v3deep_orig_before != orig_.data()),
                   int(this->eval.data() == orig_.data()),
                   int(this->eval.data() == eval_mut.data()));
    }
  }
}

void PositionDeformData::deform(MutableSpan<float3> translations, const Span<int> verts) const
{
  PRF_scope(ProfileCategory::Editor);
  if (base_recording_blocked_) {
    return;
  }
  if (base_layer_) {
    Array<float3> layer_translations(translations.as_span());
    if (deform_imats_) {
      apply_crazyspace_to_translations(*deform_imats_, verts, layer_translations);
    }
    if (!bke::multires_layers::base_deltas_record(*base_layer_, verts, layer_translations)) {
      return;
    }
    if (eval_mut_) {
      apply_translations(translations, verts, *eval_mut_);
    }
    return;
  }
  if (eval_mut_) {
    /* Apply translations to the evaluated mesh. This is necessary because multiple brush
     * evaluations can happen in between object reevaluations (otherwise just deforming the
     * original positions would be enough). */
    apply_translations(translations, verts, *eval_mut_);
  }

  if (deform_imats_) {
    /* Apply the reverse procedural deformation, since subsequent translation happens to the state
     * from "before" deforming modifiers. */
    apply_crazyspace_to_translations(*deform_imats_, verts, translations);
  }

  if (shape_key_data_) {
    if (!shape_key_data_->dependent_keys.is_empty()) {
      for (MutableSpan<float3> data : shape_key_data_->dependent_keys) {
        apply_translations(translations, verts, data);
      }
    }

    if (shape_key_data_->basis_key_active) {
      /* The basis key positions and the mesh positions are always kept in sync. */
      apply_translations(translations, verts, orig_);
    }
    apply_translations(translations, verts, shape_key_data_->active_key_data);
  }
  else {
    apply_translations(translations, verts, orig_);
  }
}

void filter_translations(const MutableSpan<float3> translations, const Span<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : translations.index_range()) {
    if (factors[i] == 0.0f) {
      translations[i] = float3(0.0f);
    }
  }
}

void scale_translations(const MutableSpan<float3> translations, const Span<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : translations.index_range()) {
    translations[i] *= factors[i];
  }
}

void scale_translations(const MutableSpan<float3> translations, const float factor)
{
  PRF_scope(ProfileCategory::Editor);
  if (factor == 1.0f) {
    return;
  }
  for (const int i : translations.index_range()) {
    translations[i] *= factor;
  }
}

void scale_factors(const MutableSpan<float> factors, const float strength)
{
  PRF_scope(ProfileCategory::Editor);
  if (strength == 1.0f) {
    return;
  }
  for (float &factor : factors) {
    factor *= strength;
  }
}

void scale_factors(const MutableSpan<float> factors, const Span<float> strengths)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(factors.size() == strengths.size());

  for (const int i : factors.index_range()) {
    factors[i] *= strengths[i];
  }
}

void translations_from_offset_and_factors(const float3 &offset,
                                          const Span<float> factors,
                                          const MutableSpan<float3> r_translations)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(r_translations.size() == factors.size());

  for (const int i : factors.index_range()) {
    r_translations[i] = offset * factors[i];
  }
}

void translations_from_new_positions(const Span<float3> new_positions,
                                     const Span<int> verts,
                                     const Span<float3> old_positions,
                                     const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(new_positions.size() == verts.size());
  for (const int i : verts.index_range()) {
    translations[i] = new_positions[i] - old_positions[verts[i]];
  }
}

void translations_from_new_positions(const Span<float3> new_positions,
                                     const Span<float3> old_positions,
                                     const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(new_positions.size() == old_positions.size());
  for (const int i : new_positions.index_range()) {
    translations[i] = new_positions[i] - old_positions[i];
  }
}

OffsetIndices<int> create_node_vert_offsets(const Span<bke::pbvh::MeshNode> nodes,
                                            const IndexMask &node_mask,
                                            Array<int> &node_data)
{
  PRF_scope(ProfileCategory::Editor);
  node_data.reinitialize(node_mask.size() + 1);
  node_mask.foreach_index_optimized<int>(
      [&](const int i, const int pos) { node_data[pos] = nodes[i].verts().size(); });
  return offset_indices::accumulate_counts_to_offsets(node_data);
}

OffsetIndices<int> create_node_vert_offsets(const CCGKey &key,
                                            const Span<bke::pbvh::GridsNode> nodes,
                                            const IndexMask &node_mask,
                                            Array<int> &node_data)
{
  PRF_scope(ProfileCategory::Editor);
  node_data.reinitialize(node_mask.size() + 1);
  node_mask.foreach_index_optimized<int>([&](const int i, const int pos) {
    node_data[pos] = nodes[i].grids().size() * key.grid_area;
  });
  return offset_indices::accumulate_counts_to_offsets(node_data);
}

OffsetIndices<int> create_node_vert_offsets_bmesh(const Span<bke::pbvh::BMeshNode> nodes,
                                                  const IndexMask &node_mask,
                                                  Array<int> &node_data)
{
  PRF_scope(ProfileCategory::Editor);
  node_data.reinitialize(node_mask.size() + 1);
  node_mask.foreach_index([&](const int i, const int pos) {
    node_data[pos] =
        BKE_pbvh_bmesh_node_unique_verts(const_cast<bke::pbvh::BMeshNode *>(&nodes[i])).size();
  });
  return offset_indices::accumulate_counts_to_offsets(node_data);
}

GroupedSpan<int> calc_vert_neighbors(const OffsetIndices<int> faces,
                                     const Span<int> corner_verts,
                                     const GroupedSpan<int> vert_to_face,
                                     const Span<bool> hide_poly,
                                     const Span<int> verts,
                                     Vector<int> &r_offset_data,
                                     Vector<int> &r_data)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(corner_verts.size() == faces.total_size());
  r_offset_data.resize(verts.size() + 1);
  r_data.clear();
  for (const int i : verts.index_range()) {
    r_offset_data[i] = r_data.size();
    append_neighbors_to_vector(faces, corner_verts, vert_to_face, hide_poly, verts[i], r_data);
  }
  r_offset_data.last() = r_data.size();
  return GroupedSpan<int>(r_offset_data.as_span(), r_data.as_span());
}

GroupedSpan<int> calc_vert_neighbors(const SubdivCCG &subdiv_ccg,
                                     const Span<int> grids,
                                     Vector<int> &r_offset_data,
                                     Vector<int> &r_data)
{
  PRF_scope(ProfileCategory::Editor);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  SubdivCCGNeighbors neighbors;

  r_offset_data.resize(key.grid_area * grids.size() + 1);
  r_data.clear();

  for (const int i : grids.index_range()) {
    const int grid = grids[i];
    const int node_verts_start = i * key.grid_area;

    for (const short y : IndexRange(key.grid_size)) {
      for (const short x : IndexRange(key.grid_size)) {
        const int offset = CCG_grid_xy_to_index(key.grid_size, x, y);
        r_offset_data[node_verts_start + offset] = r_data.size();

        SubdivCCGCoord coord{};
        coord.grid_index = grid;
        coord.x = x;
        coord.y = y;
        BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, false, neighbors);
        for (const SubdivCCGCoord neighbor : neighbors.coords) {
          r_data.append(neighbor.to_index(key));
        }
      }
    }
  }
  r_offset_data.last() = r_data.size();
  return GroupedSpan<int>(r_offset_data.as_span(), r_data.as_span());
}

GroupedSpan<BMVert *> calc_vert_neighbors(Set<BMVert *, 0> verts,
                                          Vector<int> &r_offset_data,
                                          Vector<BMVert *> &r_data)
{
  PRF_scope(ProfileCategory::Editor);
  r_offset_data.resize(verts.size() + 1);
  r_data.clear();

  BMeshNeighborVerts neighbor_data;
  int i = 0;
  for (BMVert *vert : verts) {
    r_offset_data[i] = r_data.size();
    r_data.extend(vert_neighbors_get_bmesh(*vert, neighbor_data));
    i++;
  }
  r_offset_data.last() = r_data.size();
  return GroupedSpan<BMVert *>(r_offset_data.as_span(), r_data.as_span());
}

template<bool use_factors>
static GroupedSpan<int> calc_vert_neighbors_interior_impl(const OffsetIndices<int> faces,
                                                          const Span<int> corner_verts,
                                                          const GroupedSpan<int> vert_to_face,
                                                          const BitSpan boundary_verts,
                                                          const Set<OrderedEdge> &boundary_edges,
                                                          const Span<bool> hide_poly,
                                                          const Span<int> verts,
                                                          const Span<float> factors,
                                                          Vector<int> &r_offset_data,
                                                          Vector<int> &r_data)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(corner_verts.size() == faces.total_size());
  if constexpr (use_factors) {
    BLI_assert(verts.size() == factors.size());
  }

  r_offset_data.resize(verts.size() + 1);
  r_data.clear();

  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    const int vert_start = r_data.size();
    r_offset_data[i] = vert_start;
    if constexpr (use_factors) {
      if (factors[i] == 0.0f) {
        continue;
      }
    }
    append_neighbors_to_vector(faces, corner_verts, vert_to_face, hide_poly, vert, r_data);

    if (boundary_verts[vert]) {
      /* Do not include neighbors of corner vertices. */
      if (r_data.size() == vert_start + 2) {
        r_data.resize(vert_start);
      }
      else {
        /* Only include other boundary vertices as neighbors of boundary vertices. */
        for (int neighbor_i = r_data.size() - 1; neighbor_i >= vert_start; neighbor_i--) {
          OrderedEdge edge(r_data[neighbor_i], vert);
          if (!boundary_edges.contains(OrderedEdge(r_data[neighbor_i], vert))) {
            r_data.remove_and_reorder(neighbor_i);
          }
        }
      }
    }
  }
  r_offset_data.last() = r_data.size();
  return GroupedSpan<int>(r_offset_data.as_span(), r_data.as_span());
}

GroupedSpan<int> calc_vert_neighbors_interior(const OffsetIndices<int> faces,
                                              const Span<int> corner_verts,
                                              const GroupedSpan<int> vert_to_face,
                                              const BitSpan boundary_verts,
                                              const Set<OrderedEdge> &boundary_edges,
                                              const Span<bool> hide_poly,
                                              const Span<int> verts,
                                              const Span<float> factors,
                                              Vector<int> &r_offset_data,
                                              Vector<int> &r_data)
{
  return calc_vert_neighbors_interior_impl<true>(faces,
                                                 corner_verts,
                                                 vert_to_face,
                                                 boundary_verts,
                                                 boundary_edges,
                                                 hide_poly,
                                                 verts,
                                                 factors,
                                                 r_offset_data,
                                                 r_data);
}

GroupedSpan<int> calc_vert_neighbors_interior(const OffsetIndices<int> faces,
                                              const Span<int> corner_verts,
                                              const GroupedSpan<int> vert_to_face,
                                              const BitSpan boundary_verts,
                                              const Set<OrderedEdge> &boundary_edges,
                                              const Span<bool> hide_poly,
                                              const Span<int> verts,
                                              Vector<int> &r_offset_data,
                                              Vector<int> &r_data)
{
  return calc_vert_neighbors_interior_impl<false>(faces,
                                                  corner_verts,
                                                  vert_to_face,
                                                  boundary_verts,
                                                  boundary_edges,
                                                  hide_poly,
                                                  verts,
                                                  {},
                                                  r_offset_data,
                                                  r_data);
}

void calc_vert_neighbors_interior(const OffsetIndices<int> faces,
                                  const Span<int> corner_verts,
                                  const BitSpan boundary_verts,
                                  const Set<OrderedEdge> &boundary_edges,
                                  const SubdivCCG &subdiv_ccg,
                                  const Span<int> grids,
                                  const MutableSpan<Vector<SubdivCCGCoord>> result)
{
  PRF_scope(ProfileCategory::Editor);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  BLI_assert(grids.size() * key.grid_area == result.size());

  for (const int i : grids.index_range()) {
    const int grid = grids[i];
    const int node_verts_start = i * key.grid_area;

    /* TODO: This loop could be optimized in the future by skipping unnecessary logic for
     * non-boundary grid vertices. */
    for (const int y : IndexRange(key.grid_size)) {
      for (const int x : IndexRange(key.grid_size)) {
        const int offset = CCG_grid_xy_to_index(key.grid_size, x, y);
        const int node_vert_index = node_verts_start + offset;

        SubdivCCGCoord coord{};
        coord.grid_index = grid;
        coord.x = x;
        coord.y = y;

        SubdivCCGNeighbors neighbors;
        BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, false, neighbors);

        if (boundary::vert_is_boundary(
                faces, corner_verts, boundary_verts, boundary_edges, subdiv_ccg, coord))
        {
          if (neighbors.coords.size() == 2) {
            /* Do not include neighbors of corner vertices. */
            neighbors.coords.clear();
          }
          else {
            /* Only include other boundary vertices as neighbors of boundary vertices. */
            neighbors.coords.remove_if([&](const SubdivCCGCoord coord) {
              return !boundary::vert_is_boundary(
                  faces, corner_verts, boundary_verts, boundary_edges, subdiv_ccg, coord);
            });
          }
        }
        result[node_vert_index] = neighbors.coords;
      }
    }
  }
}

void calc_vert_neighbors_interior(const Set<BMVert *, 0> &verts,
                                  MutableSpan<Vector<BMVert *>> result)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == result.size());
  BMeshNeighborVerts neighbor_data;

  int i = 0;
  for (BMVert *vert : verts) {
    vert_neighbors_get_interior_bmesh(*vert, neighbor_data);
    result[i] = neighbor_data;
    i++;
  }
}

void calc_translations_to_plane(const Span<float3> vert_positions,
                                const Span<int> verts,
                                const float4 &plane,
                                const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : verts.index_range()) {
    const float3 &position = vert_positions[verts[i]];
    float3 closest;
    closest_to_plane_normalized_v3(closest, plane, position);
    translations[i] = closest - position;
  }
}

void calc_translations_to_plane(const Span<float3> positions,
                                const float4 &plane,
                                const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : positions.index_range()) {
    const float3 &position = positions[i];
    float3 closest;
    closest_to_plane_normalized_v3(closest, plane, position);
    translations[i] = closest - position;
  }
}

void filter_verts_outside_symmetry_area(const Span<float3> positions,
                                        const float3 &pivot,
                                        const ePaintSymmetryFlags symm,
                                        const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(positions.size() == factors.size());

  for (const int i : positions.index_range()) {
    if (!check_vertex_pivot_symmetry(positions[i], pivot, symm)) {
      factors[i] = 0.0f;
    }
  }
}

void filter_plane_trim_limit_factors(const Brush &brush,
                                     const StrokeCache &cache,
                                     const Span<float3> translations,
                                     const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  if (!(brush.flag & BRUSH_PLANE_TRIM)) {
    return;
  }
  const float threshold = cache.radius_squared * cache.plane_trim_squared;
  for (const int i : translations.index_range()) {
    if (math::length_squared(translations[i]) > threshold) {
      factors[i] = 0.0f;
    }
  }
}

void filter_below_plane_factors(const Span<float3> vert_positions,
                                const Span<int> verts,
                                const float4 &plane,
                                const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : verts.index_range()) {
    if (plane_point_side_v3(plane, vert_positions[verts[i]]) <= 0.0f) {
      factors[i] = 0.0f;
    }
  }
}

void filter_below_plane_factors(const Span<float3> positions,
                                const float4 &plane,
                                const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : positions.index_range()) {
    if (plane_point_side_v3(plane, positions[i]) <= 0.0f) {
      factors[i] = 0.0f;
    }
  }
}

void filter_above_plane_factors(const Span<float3> vert_positions,
                                const Span<int> verts,
                                const float4 &plane,
                                const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : verts.index_range()) {
    if (plane_point_side_v3(plane, vert_positions[verts[i]]) > 0.0f) {
      factors[i] = 0.0f;
    }
  }
}

void filter_above_plane_factors(const Span<float3> positions,
                                const float4 &plane,
                                const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : positions.index_range()) {
    if (plane_point_side_v3(plane, positions[i]) > 0.0f) {
      factors[i] = 0.0f;
    }
  }
}

void mask_overlay_check(bContext &C, wmOperator &op)
{
  View3D *v3d = CTX_wm_view3d(&C);
  if (!v3d) {
    return;
  }

  if (v3d->flag2 & V3D_HIDE_OVERLAYS) {
    BKE_report(op.reports, RPT_WARNING, RPT_("Viewport overlays are disabled"));
  }
  else {
    if (!(v3d->overlay.flag & V3D_OVERLAY_SCULPT_SHOW_MASK)) {
      v3d->overlay.flag |= V3D_OVERLAY_SCULPT_SHOW_MASK;
      WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
    }

    if (v3d->overlay.sculpt_mode_mask_opacity == 0.0f) {
      BKE_report(op.reports, RPT_WARNING, RPT_("Mask overlay opacity is currently set to 0"));
    }
  }
}

void face_set_overlay_check(bContext &C, wmOperator &op)
{
  View3D *v3d = CTX_wm_view3d(&C);
  if (!v3d) {
    return;
  }

  if (v3d->flag2 & V3D_HIDE_OVERLAYS) {
    BKE_report(op.reports, RPT_WARNING, RPT_("Viewport overlays are disabled"));
  }
  else {
    if (!(v3d->overlay.flag & V3D_OVERLAY_SCULPT_SHOW_FACE_SETS)) {
      v3d->overlay.flag |= V3D_OVERLAY_SCULPT_SHOW_FACE_SETS;
      WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
    }

    if (v3d->overlay.sculpt_mode_face_sets_opacity == 0.0f) {
      BKE_report(op.reports, RPT_WARNING, RPT_("Face Sets overlay opacity is currently set to 0"));
    }
  }
}

}  // namespace ed::sculpt_paint

}  // namespace blender
