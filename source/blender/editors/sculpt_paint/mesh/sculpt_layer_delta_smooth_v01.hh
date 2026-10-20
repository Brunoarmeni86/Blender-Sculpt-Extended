/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_eval.hh"
#include "BKE_subdiv_ccg.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "DNA_brush_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::layer_delta_smooth_v01 {
inline bool enabled()
{
  const char *v = std::getenv("BLENDER_SCULPT_LAYER_DELTA_SMOOTH");
  return v && v[0] == '1' && v[1] == '\0';
}
struct State {
  Object *object = nullptr;
  SubdivCCG *ccg = nullptr;
  const MultiresSculptLayer *layer = nullptr;
  Span<int> ptex;
  /* DELTA_SMOOTH_COARSE_V02: persistent layer grids remain at maximum resolution. */
  int stored_grid_size = 0;
  int sample_stride = 0;
  Map<int, float3> raw_offsets;
  Map<int, float3> reference;
  bool ready = false;
};
inline State &state() { static State value; return value; }
inline void reset() { state() = State{}; }
inline bool finite(const float3 &v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline bool fail(const char *reason)
{
  state().ready = false;
  std::fprintf(stderr, "[LAYER_DELTA_SMOOTH_V01] stopped=%s\n", reason);
  return false;
}
/* Must run before stroke_cache_init and stroke Undo begin. Previous pending coordinates
 * are committed in their existing frame, never by temporarily hiding/reweighting layers. */
inline bool begin(Object &object, const Brush &brush, ReportList *reports, const bool force_smooth = false)
{
  reset();
  if (!enabled() || (!force_smooth && brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_SMOOTH &&
                     brush.autosmooth_factor <= 0.0f)) { return true; }
  SculptSession *ss = object.runtime->sculpt_session;
  MultiresModifierData *mmd = ss ? const_cast<MultiresModifierData *>(ss->multires_modifier) : nullptr;
  if (!ss || !ss->subdiv_ccg || !mmd || mmd->sculpt_layers.is_empty()) { return true; }
  const MultiresSculptLayer *layer = bke::multires_layers::active(*mmd);
  const char *error = nullptr;
  if (ss->cache || ss->filter_cache) { error = "Another sculpt operation is active"; }
  else if (!layer ||
           !(layer->flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
           !std::isfinite(layer->strength) || std::abs(layer->strength) < 1e-6f)
  { error = "Delta Smooth requires a visible active layer with nonzero strength"; }
  else if (mmd->totlvl < 1 || mmd->totlvl > 13 || ss->subdiv_ccg->level < 1 ||
           ss->subdiv_ccg->level > mmd->totlvl ||
           ss->subdiv_ccg->level != mmd->sculptlvl || layer->level != mmd->totlvl ||
           layer->grids_num != ss->subdiv_ccg->grids_num ||
           ss->subdiv_ccg->grid_size != bke::subdiv::grid_size_from_level(mmd->sculptlvl) ||
           ss->subdiv_ccg->grid_area != ss->subdiv_ccg->grid_size * ss->subdiv_ccg->grid_size)
  { error = "Delta Smooth requires a valid Multires level and maximum-resolution layer grids"; }
  else if ((mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh) ||
           mmd->sculpt_displacement_enabled || mmd->sculpt_shrinkwrap_enabled)
  { error = "Delta Smooth V01 does not support Sculpt Base Mesh or live displacement/shrinkwrap"; }
  if (!error && !force_smooth && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SMOOTH &&
      brush.smooth_deform_type != BRUSH_SMOOTH_DEFORM_LAPLACIAN) {
    error = "Delta Smooth V01 supports Laplacian Smooth only";
  }
  if (!error && layer->base_deltas) {
    for (int i = 0; i < layer->base_verts_num; i++) {
      if (layer->base_deltas[i][0] != 0.0f || layer->base_deltas[i][1] != 0.0f ||
          layer->base_deltas[i][2] != 0.0f)
      { error = "Delta Smooth V01 cannot isolate an active layer with L0 cage edits"; break; }
    }
  }
  if (error) { BKE_report(reports, RPT_ERROR, error); return false; }
  SubdivCCG *ccg = ss->subdiv_ccg;
  const bool pending = ccg->dirty.coords;
  bool preserved = false;
  if (!bke::multires_layers::commit(object, &preserved)) {
    BKE_report(reports, RPT_ERROR, "Cannot commit pending sculpt before Delta Smooth");
    return false;
  }
  if (object.runtime->sculpt_session != ss || ss->subdiv_ccg != ccg) {
    BKE_report(reports, RPT_ERROR, "Delta Smooth surface changed while committing pending sculpt");
    return false;
  }
  if (pending && !preserved) {
    /* Lower-level edits use native detail-preserving reshape at Commit. Reattach
     * its newly stored layer data and refresh the current CCG before capturing
     * a new stroke reference. This runs before stroke cache/Undo initialization.
     * Keep the established maximum-level preservation requirement unchanged. */
    if (ccg->level >= mmd->totlvl || !multires_refresh_sculpt_ccg(&object, mmd) ||
        object.runtime->sculpt_session != ss || ss->subdiv_ccg != ccg)
    {
      BKE_report(reports, RPT_ERROR, "Cannot refresh Multires after lower-level sculpt Commit");
      return false;
    }
  }
  layer = bke::multires_layers::active(*mmd);
  if (!ccg->subdiv || !layer || !layer->disps) {
    BKE_report(reports, RPT_ERROR, "Delta Smooth requires valid active-layer grids and an evaluator");
    return false;
  }
  State &s = state();
  s.ptex = bke::subdiv::face_ptex_offset_get(ccg->subdiv);
  if (s.ptex.size() != ccg->faces.size() + 1) { reset(); return false; }
  s.stored_grid_size = bke::subdiv::grid_size_from_level(layer->level);
  s.sample_stride = (s.stored_grid_size - 1) / (ccg->grid_size - 1);
  if (s.sample_stride < 1 ||
      (ccg->grid_size - 1) * s.sample_stride != s.stored_grid_size - 1)
  { reset(); return false; }
  s.object = &object; s.ccg = ccg; s.layer = layer; s.ready = true;
  std::fprintf(stderr, "[LAYER_DELTA_SMOOTH_V01] begin level=%d strength=%.6f pending_commit=%d\n",
               ccg->level, layer->strength, int(pending));
  return true;
}
/* Evaluator calls and all map insertions stay serial. Workers only read captured references. */
inline bool raw_offset(const int index, float3 &out)
{
  State &s = state();
  if (const float3 *p = s.raw_offsets.lookup_ptr(index)) { out = *p; return true; }
  const SubdivCCG &ccg = *s.ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(ccg);
  const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, index);
  if (coord.grid_index < 0 || coord.grid_index >= s.layer->grids_num) { return fail("grid_index"); }
  const MDisps &grid = s.layer->disps[coord.grid_index];
  if (grid.level != s.layer->level ||
      grid.totdisp != s.stored_grid_size * s.stored_grid_size)
  { return fail("grid_descriptor"); }
  if (!grid.disps) { out = float3(0.0f); s.raw_offsets.add(index, out); return true; }
  const int face_index = BKE_subdiv_ccg_grid_to_face_index(ccg, coord.grid_index);
  const IndexRange face = ccg.faces[face_index];
  const int corner = coord.grid_index - face.start();
  const float gu = float(coord.x) / float(key.grid_size - 1);
  const float gv = float(coord.y) / float(key.grid_size - 1);
  /* Multires grids are nested dyadic lattices. Every current-level sample is
   * exactly a stored-level sample: use an integer stride, no layer resampling,
   * no bilinear rounding and no downsampling of persistent high detail. */
  const int stored_x = int(coord.x) * s.sample_stride;
  const int stored_y = int(coord.y) * s.sample_stride;
  const float *sample = grid.disps[stored_y * s.stored_grid_size + stored_x];
  const float3 delta = float3(sample[0], sample[1], sample[2]) * s.layer->strength;
  if (!finite(delta)) { return fail("nonfinite_layer_sample"); }
  if (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f) {
    out = float3(0.0f); s.raw_offsets.add(index, out); return true;
  }
  const bool quad = face.size() == 4;
  const int ptex = s.ptex[face_index] + (quad ? 0 : corner);
  float u, v;
  if (quad) { bke::subdiv::rotate_grid_to_quad(corner, gu, gv, &u, &v); }
  else { bke::subdiv::grid_uv_to_ptex_face_uv(gu, gv, &u, &v); }
  float3 P, du, dv;
  bke::subdiv::eval_limit_point_and_derivatives(ccg.subdiv, ptex, u, v, P, du, dv);
  float3x3 tangent;
  BKE_multires_construct_tangent_matrix(tangent, du, dv, quad ? corner : 0);
  out = math::transform_direction(tangent, delta);
  if (!finite(out)) { return fail("nonfinite_offset"); }
  s.raw_offsets.add(index, out);
  return true;
}
inline bool capture(const int index)
{
  State &s = state();
  if (s.reference.contains(index)) { return true; }
  const SubdivCCG &ccg = *s.ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(ccg);
  const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, index);
  SubdivCCGNeighbors neighbors;
  if (coord.x == 0 || coord.y == 0 || coord.x == key.grid_size - 1 ||
      coord.y == key.grid_size - 1) {
    BKE_subdiv_ccg_neighbor_coords_get(ccg, coord, true, neighbors);
  }
  else { neighbors.num_duplicates = 0; }
  Vector<int, 16> aliases;
  aliases.append(index);
  for (const SubdivCCGCoord duplicate : neighbors.duplicates()) {
    aliases.append(duplicate.to_index(key));
  }
  /* Identical accumulation order on every copy of a stitched physical sample. */
  std::sort(aliases.begin(), aliases.end());
  float3 offset(0.0f);
  for (const int alias : aliases) {
    float3 other;
    if (!raw_offset(alias, other)) { return false; }
    offset += other;
  }
  offset /= float(aliases.size());
  const float3 ref = ccg.positions[index] - offset;
  if (!finite(ref)) { return fail("nonfinite_reference"); }
  s.reference.add(index, ref);
  return true;
}
/* Called before the main brush action too: Auto Smooth must retain its newly drawn delta. */
inline bool prepare_region(Object &object, const IndexMask &node_mask)
{
  State &s = state();
  if (s.object != &object) { return true; }
  if (!s.ready) { return false; }
  if (object.runtime->sculpt_session->subdiv_ccg != s.ccg) { return fail("surface_changed"); }
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const CCGKey key = BKE_subdiv_ccg_key_top_level(*s.ccg);
  bool ok = true;
  node_mask.foreach_index([&](const int n) {
    if (!ok) { return; }
    for (const int grid : nodes[n].grids()) {
      for (int j = 0; j < key.grid_area; j++) {
        if (!capture(grid * key.grid_area + j)) { ok = false; return; }
      }
      for (int y = 0; y < key.grid_size; y++) {
        for (int x = 0; x < key.grid_size; x++) {
          if (x != 0 && y != 0 && x != key.grid_size - 1 && y != key.grid_size - 1) { continue; }
          const SubdivCCGCoord coord{grid, short(x), short(y)};
          SubdivCCGNeighbors neighbors;
          BKE_subdiv_ccg_neighbor_coords_get(*s.ccg, coord, false, neighbors);
          for (const SubdivCCGCoord neighbor : neighbors.coords) {
            if (!capture(neighbor.to_index(key))) { ok = false; return; }
          }
        }
      }
    }
  });
  return ok;
}
inline bool owns(const Object &object) { return state().object == &object; }
}  // namespace blender::ed::sculpt_paint::layer_delta_smooth_v01

