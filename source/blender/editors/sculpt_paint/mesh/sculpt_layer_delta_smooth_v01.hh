/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "BLI_listbase.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_multires_layer_smooth_reference.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_eval.hh"
#include "BKE_subdiv_ccg.hh"
#include "BLI_map.hh"
#include "BLI_bit_vector.hh"
#include "BLI_bit_span_ops.hh"
#include "BLI_set.hh"
#include "BKE_sculpt_cold_start_profile.hh"
#include "BLI_math_matrix.hh"
#include "DNA_brush_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "sculpt_intern.hh"
#include "RNA_access.hh"

namespace blender::ed::sculpt_paint::layer_delta_smooth_v01 {
inline bool enabled()
{
  const char *v = std::getenv("BLENDER_SCULPT_LAYER_DELTA_SMOOTH");
  return v && v[0] == '1' && v[1] == '\0';
}
/* Full grids in the actual brush region use contiguous values and validity bits.
 * Neighbor-only halo samples remain sparse: touching one seam must not allocate
 * a complete high-resolution neighboring grid. Writes finish before worker reads. */
class SampleStore {
  int area_ = 0;
  bool compact_ = false;
  Map<int, int64_t> grids_;
  Vector<float3> values_;
  BitVector<> valid_;
  Map<int, float3> sparse_;
 public:
  void configure(const int area)
  {
    BLI_assert(area_ == 0 || area_ == area);
    if (area_ != 0) { return; }
    area_ = area;
    const char *env = std::getenv("BLENDER_SCULPT_DELTA_COMPACT_BUFFERS");
    /* Tiny L1/L2 grids do not justify a second grid lookup. */
    compact_ = area >= 25 && (!env || env[0] != '0');
  }
  void prepare_grid(const int grid)
  {
    if (!compact_ || grids_.contains(grid)) { return; }
    const int64_t offset = values_.size();
    grids_.add(grid, offset);
    values_.resize(offset + area_);
    valid_.resize(offset + area_, false);
  }
  const float3 *lookup_ptr(const int index) const
  {
    if (compact_) {
      if (const int64_t *offset = grids_.lookup_ptr(index / area_)) {
        const int64_t i = *offset + index % area_;
        if (valid_[i]) { return &values_[i]; }
      }
    }
    return sparse_.lookup_ptr(index);
  }
  /* Read only after the caller has completed every sample of this grid. */
  const float3 *prepared_grid_data(const int grid) const
  {
    if (compact_) {
      if (const int64_t *offset = grids_.lookup_ptr(grid)) { return values_.data() + *offset; }
    }
    return nullptr;
  }
  /* A formerly halo-only grid can retain sparse entries after promotion.
   * Only expose a direct span when every dense sample is actually initialized. */
  const float3 *complete_grid_data(const int grid) const
  {
    if (compact_) {
      if (const int64_t *offset = grids_.lookup_ptr(grid)) {
        const BoundedBitSpan valid = valid_;
        if (!bits::any_bit_unset(valid.slice(IndexRange(*offset, area_)))) {
          return values_.data() + *offset;
        }
      }
    }
    return nullptr;
  }
  bool contains(const int index) const { return lookup_ptr(index) != nullptr; }
  const float3 &lookup(const int index) const
  {
    const float3 *value = lookup_ptr(index);
    BLI_assert(value != nullptr);
    return *value;
  }
  void add(const int index, const float3 &value)
  {
    if (compact_) {
      if (const int64_t *offset = grids_.lookup_ptr(index / area_)) {
        const int64_t i = *offset + index % area_;
        BLI_assert(!valid_[i]);
        values_[i] = value;
        valid_[i].set();
        return;
      }
    }
    sparse_.add(index, value);
  }
};
struct State {
  Object *object = nullptr;
  /* Freeze the routing decision before Shift switches to the Smooth asset. */
  Object *native_object = nullptr;
  Array<float3> mesh_reference;
  bool mesh_l0 = false;
  bool persistent_coarse_reference = false;
  SubdivCCG *ccg = nullptr;
  const MultiresSculptLayer *layer = nullptr;
  Span<int> ptex;
  /* Borrowed from this live CCG's single coarse reference; no per-stroke topology. */
  const bke::multires_layers::SmoothL0Reference *base_delta_reference = nullptr;
  /* DELTA_SMOOTH_COARSE_V02: persistent layer grids remain at maximum resolution. */
  int stored_grid_size = 0;
  int sample_stride = 0;
  SampleStore raw_offsets;
  SampleStore reference;
  Set<int> prepared_grids;
  bool reuse_prepared = true;
  bool ready = false;
};
inline State &state() { static State value; return value; }
inline void reset() { state() = State{}; }
inline bool isolation_requested(const Brush &brush)
{
  PointerRNA ptr = RNA_id_pointer_create(const_cast<ID *>(&brush.id));
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "sculpt_preserve_other_layers");
  /* Preserve the established Delta Smooth policy if startup RNA is unavailable. */
  return prop == nullptr || RNA_property_boolean_get(&ptr, prop);
}
inline bool uses_native_smooth(const Object &object)
{
  return state().native_object == &object;
}
inline bool targets_layer(const MultiresModifierData &mmd)
{
  return !(mmd.flags & eMultiresModifierFlag_UseSculptBaseMesh) &&
         bke::multires_layers::base_has_visible_layers(mmd);
}
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
/* L0 is propagated into one numeric reference owned by the live CCG. Exact
 * input comparison makes unchanged strokes cheap and detects L0/weight edits. */
inline bool prepare_base_delta(State &s, Object &object)
{
  if (!s.layer->base_deltas) { return true; }
  const auto *mmd = object.runtime->sculpt_session->multires_modifier;
  const Mesh *mesh = BKE_mesh_from_object(&object);
  if (!mmd || mmd->modifier.prev != nullptr || !mesh) { return false; }
  return bke::multires_layers::smooth_l0_reference_prepare(
      *s.layer, *mesh, *s.ccg, s.base_delta_reference);
}
/* Must run before stroke_cache_init and stroke Undo begin. Previous pending coordinates
 * are committed in their existing frame, never by temporarily hiding/reweighting layers. */
/* Coarse layer isolation uses vertex deltas directly, never a temporary subdivision. */
inline bool mesh_protected_offsets(Object &object, const MultiresModifierData &mmd,
                                   Array<float3> &offsets, ReportList *reports)
{
  const Mesh &mesh = *id_cast<Mesh *>(object.data);
  const auto *active = bke::multires_layers::active(mmd);
  if (!active || !bke::multires_layers::can_record(mmd, mesh) ||
      mmd.modifier.prev || mmd.modifier.next || mmd.sculpt_displacement_enabled ||
      mmd.sculpt_shrinkwrap_enabled) {
    BKE_report(reports, RPT_ERROR, "L0 layer isolation requires a valid active layer and Multires as the only modifier");
    return false;
  }
  offsets.reinitialize(mesh.verts_num); offsets.fill(float3(0));
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (&layer == active || !bke::multires_layers::is_effective(layer) || !layer.base_deltas) { continue; }
    if (layer.base_verts_num != mesh.verts_num) { return false; }
    for (const int i : offsets.index_range()) {
      offsets[i] += float3(layer.base_deltas[i]) * layer.strength;
      if (!finite(offsets[i])) { return false; }
    }
  }
  return true;
}
inline bool begin(Object &object, const Brush &brush, ReportList *reports, const bool force_smooth = false)
{
  reset();
  if (!enabled() || (!force_smooth && brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_SMOOTH &&
                     brush.autosmooth_factor <= 0.0f)) { return true; }
  /* Disabling isolation must bypass both its preflight and its brush dispatcher.
   * L0 layer data remains untouched and is handled by ordinary Multires Commit. */
  if (!isolation_requested(brush)) {
    state().native_object = &object;
    return true;
  }
  SculptSession *ss = object.runtime->sculpt_session;
  MultiresModifierData *mmd = ss ? const_cast<MultiresModifierData *>(ss->multires_modifier) : nullptr;
  if (ss && !ss->subdiv_ccg) {
    if (auto *base = bke::multires_layers::base_modifier(object)) {
      if (base->sculptlvl == 0 && targets_layer(*base)) {
        State &s = state();
        if (!mesh_protected_offsets(object, *base, s.mesh_reference, reports)) { reset(); return false; }
        const Span<float3> positions = id_cast<Mesh *>(object.data)->vert_positions();
        for (const int i : s.mesh_reference.index_range()) { s.mesh_reference[i] += positions[i]; }
        s.object = &object; s.mesh_l0 = true; s.ready = true;
        return true;
      }
    }
  }
  if (!ss || !ss->subdiv_ccg || !mmd) { return true; }
  /* Delta Smooth isolates the active layer from the evaluated surface. When
   * Sculpt Base Mesh is the selected target there is no layer to isolate:
   * leave this state empty and let Blender's regular Smooth brush operate on
   * the base mesh, regardless of hidden or zero-strength layers. */
  if (!targets_layer(*mmd)) { return true; }
  if (mmd->sculpt_layers.is_empty()) { return true; }
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
  else if (mmd->sculpt_displacement_enabled || mmd->sculpt_shrinkwrap_enabled)
  { error = "Delta Smooth V01 does not support live displacement/shrinkwrap"; }
  if (!error && !force_smooth && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_SMOOTH &&
      brush.smooth_deform_type != BRUSH_SMOOTH_DEFORM_LAPLACIAN) {
    error = "Delta Smooth V01 supports Laplacian Smooth only";
  }
  if (error) { BKE_report(reports, RPT_ERROR, error); return false; }
  SubdivCCG *ccg = ss->subdiv_ccg;
  const bool pending = ccg->dirty.coords;
  const char *coarse_env = std::getenv("BLENDER_SCULPT_COARSE_SMOOTH_REFERENCE_V01");
  const bool coarse_reference = coarse_env && coarse_env[0] == '1' && coarse_env[1] == '\0' &&
      ccg->level < mmd->totlvl && ccg->subdiv && ccg->subdiv->displacement_evaluator &&
      mmd->modifier.prev == nullptr && mmd->modifier.next == nullptr;
  /* The persistent evaluator does not contain pending CCG edits. Sampling it
   * minus the stored active layer gives the protected reference even across
   * consecutive strokes, without publishing a full-resolution reshape here.
   * Keep dirty flags and the change journal intact for normal commit/Undo. */
  if (!coarse_reference) {
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
      if (!multires_refresh_sculpt_ccg(&object, mmd) ||
          object.runtime->sculpt_session != ss || ss->subdiv_ccg != ccg)
      {
        BKE_report(reports, RPT_ERROR, "Cannot refresh Multires after sculpt Commit");
        return false;
      }
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
  s.object = &object; s.ccg = ccg; s.layer = layer;
  s.persistent_coarse_reference = coarse_reference;
  s.raw_offsets.configure(ccg->grid_area);
  s.reference.configure(ccg->grid_area);
  const char *reuse = std::getenv("BLENDER_SCULPT_DELTA_PREPARE_REUSE");
  s.reuse_prepared = !reuse || reuse[0] != '0';
  if (!prepare_base_delta(s, object)) {
    reset();
    BKE_report(reports, RPT_ERROR, "Cannot prepare L0 layer isolation; requires a finite compatible cage and Multires first in the modifier stack");
    return false;
  }
  s.ready = true;
  std::fprintf(stderr, "[LAYER_DELTA_SMOOTH_V01] begin level=%d strength=%.6f pending_commit=%d persistent_reference=%d\n",
               ccg->level, layer->strength, int(pending), int(coarse_reference));
  return true;
}
/* Evaluator calls and all map insertions stay serial. Workers only read captured references. */
/* Optional scratch is shared only by callers sampling the same CCG coordinate.
 * It is local to that sample, never retained across layer edits or strokes. */
inline bool raw_offset(State &s, const int index, float3 &out,
                       float3x3 *shared_tangent = nullptr, bool *shared_tangent_ready = nullptr)
{
  const auto invalid = [&](const char *reason) {
    s.ready = false;
    std::fprintf(stderr, "[LAYER_OFFSET] stopped=%s\n", reason);
    return false;
  };
  s.raw_offsets.configure(s.ccg->grid_area);
  if (const float3 *p = s.raw_offsets.lookup_ptr(index)) { out = *p; return true; }
  const SubdivCCG &ccg = *s.ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(ccg);
  const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, index);
  if (coord.grid_index < 0 || coord.grid_index >= s.layer->grids_num) { return invalid("grid_index"); }
  const MDisps &grid = s.layer->disps[coord.grid_index];
  if (grid.level != s.layer->level ||
      grid.totdisp != s.stored_grid_size * s.stored_grid_size)
  { return invalid("grid_descriptor"); }
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
  float3 delta(0.0f);
  if (grid.disps) {
    const float *sample = grid.disps[stored_y * s.stored_grid_size + stored_x];
    delta = float3(sample[0], sample[1], sample[2]) * s.layer->strength;
    if (!finite(delta)) { return invalid("nonfinite_layer_sample"); }
  }
  const bool quad = face.size() == 4;
  const int ptex = s.ptex[face_index] + (quad ? 0 : corner);
  float u, v;
  if (quad) { bke::subdiv::rotate_grid_to_quad(corner, gu, gv, &u, &v); }
  else { bke::subdiv::grid_uv_to_ptex_face_uv(gu, gv, &u, &v); }
  const float3 base_offset = s.base_delta_reference ?
      bke::multires_layers::smooth_l0_reference_sample(*s.base_delta_reference, ptex, u, v) :
      float3(0.0f);
  if (!finite(base_offset)) { return invalid("nonfinite_base_offset"); }
  if (delta == float3(0.0f)) {
    out = base_offset; s.raw_offsets.add(index, out); return true;
  }
  float3x3 local_tangent;
  float3x3 &tangent = shared_tangent ? *shared_tangent : local_tangent;
  if (!shared_tangent_ready || !*shared_tangent_ready) {
    float3 P, du, dv;
    bke::subdiv::eval_limit_point_and_derivatives(ccg.subdiv, ptex, u, v, P, du, dv);
    BKE_multires_construct_tangent_matrix(tangent, du, dv, quad ? corner : 0);
    if (shared_tangent_ready) { *shared_tangent_ready = true; }
  }
  out = base_offset + math::transform_direction(tangent, delta);
  if (!finite(out)) { return invalid("nonfinite_offset"); }
  s.raw_offsets.add(index, out);
  return true;
}
inline bool raw_offset(const int index, float3 &out)
{
  return raw_offset(state(), index, out);
}
/* Evaluate the committed surface, never the live coordinates being edited. */
inline bool persistent_position(State &s, const int index, float3 &out)
{
  const SubdivCCG &ccg = *s.ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(ccg);
  const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, index);
  const int face_index = BKE_subdiv_ccg_grid_to_face_index(ccg, coord.grid_index);
  const IndexRange face = ccg.faces[face_index];
  const int corner = coord.grid_index - face.start();
  const float gu = float(coord.x) / float(key.grid_size - 1);
  const float gv = float(coord.y) / float(key.grid_size - 1);
  const bool quad = face.size() == 4;
  const int ptex = s.ptex[face_index] + (quad ? 0 : corner);
  float u, v;
  if (quad) { bke::subdiv::rotate_grid_to_quad(corner, gu, gv, &u, &v); }
  else { bke::subdiv::grid_uv_to_ptex_face_uv(gu, gv, &u, &v); }
  out = bke::subdiv::eval_final_point(ccg.subdiv, ptex, u, v);
  return finite(out);
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
  float3 offset(0.0f), persistent(0.0f);
  for (const int alias : aliases) {
    float3 other;
    if (!raw_offset(alias, other)) { return false; }
    offset += other;
    if (s.persistent_coarse_reference) {
      float3 point;
      if (!persistent_position(s, alias, point)) { return fail("nonfinite_persistent_reference"); }
      persistent += point;
    }
  }
  offset /= float(aliases.size());
  const float3 surface = s.persistent_coarse_reference ?
      persistent / float(aliases.size()) : ccg.positions[index];
  const float3 ref = surface - offset;
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
  if (s.mesh_l0) { return true; }
  if (object.runtime->sculpt_session->subdiv_ccg != s.ccg) { return fail("surface_changed"); }
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const CCGKey key = BKE_subdiv_ccg_key_top_level(*s.ccg);
  bke::sculpt_cold_v01::Scope profile("smooth.prepare_region", &object);
  /* Promote only grids in the node mask, never the external neighbor halo. */
  node_mask.foreach_index([&](const int n) {
    for (const int grid : nodes[n].grids()) {
      s.raw_offsets.prepare_grid(grid);
      s.reference.prepare_grid(grid);
    }
  });
  bool ok = true;
  node_mask.foreach_index([&](const int n) {
    if (!ok) { return; }
    for (const int grid : nodes[n].grids()) {
      if (s.reuse_prepared && s.prepared_grids.contains(grid)) { continue; }
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
      if (ok && s.reuse_prepared) { s.prepared_grids.add(grid); }
    }
  });
  return ok;
}
inline bool owns(const Object &object) { return state().object == &object; }
}  // namespace blender::ed::sculpt_paint::layer_delta_smooth_v01

