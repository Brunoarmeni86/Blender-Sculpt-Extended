/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"

#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.h"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_object.hh"
#include "BKE_subdiv.hh"

#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>
#include "BLI_bitmap.h"
#include "BLI_path_utils.hh"
#include "BLI_time.h"
#include <cstdio>
#include "BLI_listbase.h"
#include "MEM_guardedalloc.h"

#include "DEG_depsgraph_query.hh"

#include "multires_reshape.hh"
#include "multires_conform_rebase.hh"
#include "BLI_memory_utils.hh"
#include "BLI_task.hh"
#include <atomic>
#include <cstdlib>
#include "multires_subdivide_work_v01.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "DNA_object_types.h"

namespace blender {

/* SUBDIVIDE_COMPACT_PHASES_V1: diagnostic only, no geometry changes. */
struct MultiresSubdivisionPhaseTimer {
  const char *phase;
  const char *role;
  int level;
  int corners;
  const char *path;
  bool enabled;
  double start;
  MultiresSubdivisionPhaseTimer(const char *phase, const char *role, int level, int corners)
      : phase(phase), role(role), level(level), corners(corners),
        path(BLI_getenv("BLENDER_SUBDIV_PROFILE_LOG")),
        enabled((path && path[0]) || BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PROFILE")),
        start(enabled ? BLI_time_now_seconds() : 0.0)
  {
  }
  ~MultiresSubdivisionPhaseTimer()
  {
    if (!enabled) {
      return;
    }
    const double ms = (BLI_time_now_seconds() - start) * 1000.0;
    FILE *out = path && path[0] ? std::fopen(path, "a") : nullptr;
    FILE *stream = out ? out : stdout;
    std::fprintf(stream,
                 "[SUBDIV_PHASE] phase=%s role=%s level=%d corners=%d ms=%.3f\n",
                 phase, role, level, corners, ms);
    std::fflush(stream);
    if (out) {
      std::fclose(out);
    }
  }
};


/* SUBDIVIDE_WORK_V01: independent grid work only. Never run complete native
 * subdivision/evaluator/mesh copies for multiple layers concurrently. */
static bool multires_subdivide_flag_v01(const char *name)
{
  const char *value = BLI_getenv(name);
  return value && value[0] == '1' && value[1] == '\0';
}

static int multires_subdivide_lanes_v01(const int grids, const int area)
{
  if (!multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_PARALLEL_V01")) {
    return 1;
  }
  const char *setting = BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_WORKERS_V01");
  int workers = 4;
  if (setting && setting[0]) {
    char *end = nullptr;
    const long parsed = std::strtol(setting, &end, 10);
    if (end != setting && *end == '\0' && parsed >= 1 && parsed <= 8) {
      workers = int(parsed);
    }
  }
  return bke::multires_layers::subdivide_work_v01::lanes(
      grids, area, workers, int64_t(16) * 1024 * 1024);
}

template<typename Fn>
static void multires_subdivide_grids_v01(const int grids, const int area, Fn &&fn)
{
  const int lanes = multires_subdivide_lanes_v01(grids, area);
  if (lanes == 1) {
    for (int grid = 0; grid < grids; grid++) {
      fn(grid);
    }
    return;
  }
  threading::parallel_for(IndexRange(lanes), 1, [&](const IndexRange range) {
    for (const int lane : range) {
      for (int grid = lane; grid < grids; grid += lanes) {
        fn(grid);
      }
    }
  });
}

/* -------------------------------------------------------------------- */
/** \name Reshape from object
 * \{ */

static bool multiresModifier_reshapeFromVertcos(Depsgraph *depsgraph,
                                                Object *object,
                                                MultiresModifierData *mmd,
                                                Span<float3> positions)
{
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(&reshape_context, depsgraph, object, mmd)) {
    return false;
  }
  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(id_cast<Mesh *>(object->data), reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_vertcos(&reshape_context, positions)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);
  return true;
}

bool multiresModifier_reshapeFromObject(Depsgraph *depsgraph,
                                        MultiresModifierData *mmd,
                                        Object *dst,
                                        Object *src)
{
  const Object *ob_eval = DEG_get_evaluated(depsgraph, src);
  if (!ob_eval) {
    return false;
  }
  const Mesh *src_mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (!src_mesh_eval) {
    return false;
  }

  return multiresModifier_reshapeFromVertcos(depsgraph, dst, mmd, src_mesh_eval->vert_positions());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from modifier
 * \{ */

bool multiresModifier_reshapeFromDeformModifier(Depsgraph *depsgraph,
                                                Object *object,
                                                MultiresModifierData *mmd,
                                                ModifierData *deform_md)
{
  MultiresModifierData highest_mmd = dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;

  /* Create mesh for the multires, ignoring any further modifiers (leading
   * deformation modifiers will be applied though). */
  Mesh *multires_mesh = BKE_multires_create_mesh(depsgraph, object, &highest_mmd);
  Array<float3> deformed_verts(multires_mesh->vert_positions());

  /* Apply deformation modifier on the multires, */
  ModifierEvalContext modifier_ctx{};
  modifier_ctx.depsgraph = depsgraph;
  modifier_ctx.object = object;
  modifier_ctx.flag = MOD_APPLY_USECACHE | MOD_APPLY_IGNORE_SIMPLIFY;

  const bool deform_success = BKE_modifier_deform_verts(
      deform_md, &modifier_ctx, multires_mesh, deformed_verts);
  BKE_id_free(nullptr, multires_mesh);
  if (!deform_success) {
    return false;
  }

  /* Reshaping */
  bool result = multiresModifier_reshapeFromVertcos(
      depsgraph, object, &highest_mmd, deformed_verts);

  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from grids
 * \{ */

bool multiresModifier_reshapeFromCCG(const int tot_level, Mesh *coarse_mesh, SubdivCCG *subdiv_ccg)
{
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg(
          &reshape_context, subdiv_ccg, coarse_mesh, tot_level))
  {
    return false;
  }

  multires_ensure_external_read(coarse_mesh, reshape_context.top.level);

  /* At equal levels, smoothing returns before reading original grids.
   * Avoid their deep copy only under the opt-in policy. Lower-level reshape
   * retains the original detail-preserving path. */
  const char *skip_env = BLI_getenv("BLENDER_SCULPT_COMMIT_SKIP_ORIGINALS");
  const bool skip_originals = skip_env != nullptr && skip_env[0] == '1' &&
                              skip_env[1] == '\0' &&
                              reshape_context.reshape.level == reshape_context.top.level;
  if (!skip_originals) {
    multires_reshape_store_original_grids(&reshape_context);
  }
  if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
    std::printf("[LAYER_COMMIT_ORIGINAL_GRIDS] mode=%s reshape=%d top=%d\n",
                skip_originals ? "SKIP_EQUAL_LEVEL" : "COPY",
                reshape_context.reshape.level, reshape_context.top.level);
  }
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_ccg(&reshape_context, subdiv_ccg)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Subdivision
 * \{ */

void multiresModifier_subdivide(Object *object,
                                MultiresModifierData *mmd,
                                const MultiresSubdivideModeType mode)
{
  const int top_level = mmd->totlvl + 1;
  multiresModifier_subdivide_to_level(object, mmd, top_level, mode);
}

static void multiresModifier_subdivide_to_level_base(Object *object,
                                                     MultiresModifierData *mmd,
                                                     const int top_level,
                                                     const MultiresSubdivideModeType mode)
{
  if (top_level <= mmd->totlvl) {
    return;
  }

  Mesh *coarse_mesh = id_cast<Mesh *>(object->data);
  MultiresSubdivisionPhaseTimer native_timer("native_subdivide_total", "native", top_level, coarse_mesh->corners_num);
  if (coarse_mesh->corners_num == 0) {
    /* If there are no loops in the mesh implies there is no CD_MDISPS as well. So can early output
     * from here as there is nothing to subdivide. */
    return;
  }

  MultiresReshapeContext reshape_context;

  /* There was no multires at all, all displacement is at 0. Can simply make sure all mdisps grids
   * are allocated at a proper level and return. */
  const bool has_mdisps = CustomData_has_layer(&coarse_mesh->corner_data, CD_MDISPS);
  if (!has_mdisps) {
    CustomData_add_layer(
        &coarse_mesh->corner_data, CD_MDISPS, CD_SET_DEFAULT, coarse_mesh->corners_num);
  }

  /* NOTE: Subdivision happens from the top level of the existing multires modifier. If it is set
   * to 0 and there is mdisps layer it would mean that the modifier went out of sync with the data.
   * This happens when, for example, linking modifiers from one object to another.
   *
   * In such cases simply ensure grids to be the proper level.
   *
   * If something smarter is needed it is up to the operators which does data synchronization, so
   * that the mdisps layer is also synchronized. */
  if (!has_mdisps || top_level == 1 || mmd->totlvl == 0) {
    multires_reshape_ensure_grids(coarse_mesh, top_level);
    if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
      multires_subdivide_create_tangent_displacement_linear_grids(object, mmd);
    }
    else {
      multires_set_tot_level(object, mmd, top_level);
    }
    return;
  }

  if (!multires_flush_sculpt_updates(object)) {
    return;
  }

  if (!multires_reshape_context_create_from_modifier(&reshape_context, object, mmd, top_level)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  multires_reshape_assign_final_elements_from_orig_mdisps(&reshape_context);

  /* Free original grids which makes it so smoothing with details thinks all the details were
   * added against base mesh's limit surface. This is similar behavior to as if we've done all
   * displacement in sculpt mode at the old top level and then propagated to the new top level. */
  multires_reshape_free_original_grids(&reshape_context);

  if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
    multires_reshape_smooth_object_grids(&reshape_context, mode);
  }
  else {
    multires_reshape_smooth_object_grids_with_details(&reshape_context);
  }

  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);

  multires_set_tot_level(object, mmd, top_level);
}

/* Apply the same object-space subdivision to the base and to base + each unit
 * layer. Their difference is the layer at the new resolution. Interpolating
 * tangent offsets directly bypasses Catmull-Clark smoothing and its frame
 * conversion, and leaves visible differences compared with a baked surface. */
/* Experimental retained-sample constraint. This changes only old sample locations;
 * newly inserted samples retain the existing Catmull-Clark result. Scratch-only:
 * invalid metadata or non-finite input never publishes to the live layer. */
static bool multires_preserve_retained_samples(const MDisps &base,
                                               const MDisps *layer,
                                               MDisps &result)
{
  if (base.level < 1 || base.level > 13 || result.level <= base.level ||
      result.level > 13 || !base.disps || !result.disps)
  {
    return false;
  }
  const int old_side = bke::subdiv::grid_size_from_level(base.level);
  const int new_side = bke::subdiv::grid_size_from_level(result.level);
  if (base.totdisp != old_side * old_side || result.totdisp != new_side * new_side ||
      (new_side - 1) % (old_side - 1) != 0 ||
      (layer && (layer->level != base.level || layer->totdisp != base.totdisp)))
  {
    return false;
  }
  const int stride = (new_side - 1) / (old_side - 1);
  for (int y = 0; y < old_side; y++) {
    for (int x = 0; x < old_side; x++) {
      const int src = y * old_side + x;
      const int dst = y * stride * new_side + x * stride;
      for (int axis = 0; axis < 3; axis++) {
        const float delta = layer && layer->disps ? layer->disps[src][axis] : 0.0f;
        const float value = base.disps[src][axis] + delta;
        if (!std::isfinite(base.disps[src][axis]) || !std::isfinite(delta) ||
            !std::isfinite(value))
        {
          return false;
        }
        result.disps[dst][axis] = value;
      }
    }
  }
  return true;
}

/* V02: scoped strictly to one synchronous compact subdivision transaction.
 * Only the unchanged coarse-cage evaluator is shared. Each surface retains
 * its own mutable high-resolution smoothing evaluator and scratch grids. */
struct MultiresSubdivideCoarseV02 {
  bke::subdiv::Subdiv *subdiv = nullptr;
  int uses = 0;
  MultiresSubdivideSmoothCacheV03 *smooth_cache_v03 = nullptr;
  ~MultiresSubdivideCoarseV02()
  {
    multires_subdivide_smooth_cache_free_v03(smooth_cache_v03);
    if (subdiv) {
      bke::subdiv::free(subdiv);
    }
  }
  MultiresSubdivideCoarseV02() = default;
  MultiresSubdivideCoarseV02(const MultiresSubdivideCoarseV02 &) = delete;
  MultiresSubdivideCoarseV02 &operator=(const MultiresSubdivideCoarseV02 &) = delete;
};

/* Work Mesh grid storage is already private after CustomData_get_layer_for_write.
 * Transfer its OLD displacement arrays instead of duplicating them immediately
 * before ensure_grids discards the originals. Mask copying remains unchanged.
 * Orig hidden pointers are borrowed, as in store_original_grids; its native
 * destructor only owns displacement/mask arrays, never the hidden bitmaps. */
static bool multires_subdivide_move_original_v02(MultiresReshapeContext &context)
{
  if (!multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_MOVE_SCRATCH_V02") ||
      !context.mdisps || context.orig.mdisps || context.orig.grid_paint_masks ||
      context.num_grids <= 0) {
    return false;
  }
  for (int grid = 0; grid < context.num_grids; grid++) {
    if (context.mdisps[grid].level >= context.top.level) {
      return false;
    }
  }
  /* Complete descriptor/mask allocations before transferring any ownership. */
  MDisps *original = MEM_dupalloc(context.mdisps);
  GridPaintMask *masks = context.grid_paint_masks ? MEM_dupalloc(context.grid_paint_masks) : nullptr;
  for (int grid = 0; masks && grid < context.num_grids; grid++) {
    if (masks[grid].data) {
      masks[grid].data = MEM_dupalloc(masks[grid].data);
    }
  }
  int64_t samples = 0;
  for (int grid = 0; grid < context.num_grids; grid++) {
    if (context.mdisps[grid].disps) {
      samples += context.mdisps[grid].totdisp;
    }
    context.mdisps[grid].disps = nullptr;
  }
  context.orig.mdisps = original;
  context.orig.grid_paint_masks = masks;
  if (BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PROFILE")) {
    std::printf("[SUBDIVIDE_MOVE_V02] grids=%d avoided_copy_mib=%.3f\n",
                context.num_grids, double(samples) * sizeof(float[3]) / (1024.0 * 1024.0));
  }
  return true;
}

static Mesh *multires_subdivide_layer_surface(Object *object,
                                              const MultiresModifierData &mmd,
                                              const MultiresSculptLayer *layer,
                                              const int top_level,
                                              const MultiresSubdivideModeType mode,
                                              MultiresSubdivideCoarseV02 *coarse = nullptr,
                                              MutableSpan<int8_t> regional_grids = {})
{
  const Mesh &source = *id_cast<Mesh *>(object->data);
  const char *profile_role = layer ? "layer" : "base";
  MultiresSubdivisionPhaseTimer surface_timer("surface_total", profile_role, top_level, source.corners_num);
  Mesh *work;
  {
    MultiresSubdivisionPhaseTimer timer("mesh_copy", profile_role, top_level, source.corners_num);
    work = BKE_mesh_copy_for_eval(source);
  }
  MDisps *grids = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&work->corner_data, CD_MDISPS, work->corners_num));
  {
  MultiresSubdivisionPhaseTimer timer("prepare_displacements", profile_role, top_level, source.corners_num);
  for (int corner = 0; corner < work->corners_num; corner++) {
    for (int i = 0; i < grids[corner].totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        if (layer && layer->disps && corner < layer->grids_num &&
            layer->disps[corner].disps != nullptr)
        {
          grids[corner].disps[i][axis] += layer->disps[corner].disps[i][axis];
        }
        if (!std::isfinite(grids[corner].disps[i][axis])) {
          BKE_id_free(nullptr, work);
          return nullptr;
        }
      }
    }
  }
  }
  Object work_object = dna::shallow_copy(*object);
  work_object.data = &work->id;
  MultiresModifierData work_mmd = dna::shallow_copy(mmd);
  work_mmd.sculpt_layers = {};
  MultiresReshapeContext context;
  {
  MultiresSubdivisionPhaseTimer timer("context_create", profile_role, top_level, source.corners_num);
  bool created;
  if (coarse && multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_COARSE_REUSE_V02")) {
    if (!coarse->subdiv) {
      coarse->subdiv = multires_reshape_create_subdiv(nullptr, &work_object, &work_mmd);
    }
    created = coarse->subdiv && multires_reshape_context_create_from_subdiv(
        &context, &work_object, &work_mmd, coarse->subdiv, top_level);
    if (created) {
      coarse->uses++;
    }
    if (BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PROFILE")) {
      std::printf("[SUBDIVIDE_COARSE_V02] uses=%d ok=%d\n", coarse->uses, int(created));
    }
  }
  else {
    created = multires_reshape_context_create_from_modifier(
        &context, &work_object, &work_mmd, top_level);
  }
  if (!created) {
    BKE_id_free(nullptr, work);
    return nullptr;
  }
  }
  if (coarse && multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_TOPOLOGY_V03")) {
    if (!coarse->smooth_cache_v03) {
      coarse->smooth_cache_v03 = multires_subdivide_smooth_cache_create_v03();
    }
    context.subdivide_smooth_cache_v03 = coarse->smooth_cache_v03;
  }
  if (layer && regional_grids.size() == source.corners_num &&
      mode == MultiresSubdivideModeType::CatmullClark &&
      multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_REGIONAL_V03") &&
      !multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_PRESERVE_RETAINED"))
  {
    context.regional_source_layer_v03 = layer;
    context.regional_output_grids_v03 = regional_grids;
  }
  {
  MultiresSubdivisionPhaseTimer timer("grid_allocate_assign", profile_role, top_level, source.corners_num);
  if (!multires_subdivide_move_original_v02(context)) {
    multires_reshape_store_original_grids(&context);
  }
  multires_reshape_ensure_grids(work, top_level);
  multires_reshape_assign_final_elements_from_orig_mdisps(&context);
  multires_reshape_free_original_grids(&context);
  }
  {
  MultiresSubdivisionPhaseTimer timer("smooth_details", profile_role, top_level, source.corners_num);
  if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
    multires_reshape_smooth_object_grids(&context, mode);
  }
  else {
    multires_reshape_smooth_object_grids_with_details(&context);
  }
  }
  {
  MultiresSubdivisionPhaseTimer timer("tangent_conversion", profile_role, top_level, source.corners_num);
  if (!multires_reshape_regional_object_to_tangent_v03(&context)) {
    multires_reshape_object_grids_to_tangent_displacement(&context);
  }
  multires_reshape_context_free(&context);
  }

  MultiresSubdivisionPhaseTimer hidden_timer("retained_and_hidden", profile_role, top_level, source.corners_num);
  /* ensure_grids reallocates displacement, but not the hidden bitmap. */
  const MDisps *original = static_cast<const MDisps *>(
      CustomData_get_layer(&source.corner_data, CD_MDISPS));
  const int new_side = bke::subdiv::grid_size_from_level(top_level);
  const char *preserve_env = BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PRESERVE_RETAINED");
  const bool preserve_retained = mode == MultiresSubdivideModeType::CatmullClark &&
                                preserve_env && preserve_env[0] == '1' &&
                                preserve_env[1] == '\0';
  for (int corner = 0; corner < work->corners_num; corner++) {
    if (preserve_retained) {
      const MDisps *layer_grid = layer && layer->disps ? &layer->disps[corner] : nullptr;
      if (!multires_preserve_retained_samples(original[corner], layer_grid, grids[corner])) {
        BKE_id_free(nullptr, work);
        return nullptr;
      }
    }
    MEM_SAFE_DELETE(grids[corner].hidden);
    if (!original[corner].hidden) {
      continue;
    }
    const int old_side = bke::subdiv::grid_size_from_level(original[corner].level);
    grids[corner].hidden = BLI_BITMAP_NEW(new_side * new_side, "subdivided hidden grid");
    for (int y = 0; y < new_side; y++) {
      for (int x = 0; x < new_side; x++) {
        const int sx = int(std::round(float(x) * (old_side - 1) / (new_side - 1)));
        const int sy = int(std::round(float(y) * (old_side - 1) / (new_side - 1)));
        BLI_BITMAP_SET(grids[corner].hidden, y * new_side + x,
                       BLI_BITMAP_TEST_BOOL(original[corner].hidden, sy * old_side + sx));
      }
    }
  }
  return work;
}

/* Convert the already-subdivided unit-layer surface into final deltas in
 * scratch storage. Nothing in the live layer is modified on failure. */
static bool multires_stage_subdivided_layer_serial_v01(const MultiresSculptLayer &layer,
                                           const MDisps *base,
                                           MDisps *result,
                                           const int grids_num,
                                           const int new_level,
                                           const bool allow_sparse = false)
{
  if (!base || !result || !layer.disps || layer.grids_num != grids_num ||
      layer.level < 1 || new_level <= layer.level || new_level > 13)
  {
    return false;
  }
  const int old_side = bke::subdiv::grid_size_from_level(layer.level);
  const int new_side = bke::subdiv::grid_size_from_level(new_level);
  for (int corner = 0; corner < grids_num; corner++) {
    const MDisps &old = layer.disps[corner];
    MDisps &grid = result[corner];
    if ((!old.disps && !allow_sparse) || old.level != layer.level || old.totdisp != old_side * old_side ||
        !base[corner].disps || base[corner].level != new_level ||
        base[corner].totdisp != new_side * new_side || !grid.disps ||
        grid.level != new_level || grid.totdisp != new_side * new_side)
    {
      return false;
    }
    /* Retain resample_all's validation of the old layer, including hidden layers. */
    for (int i = 0; old.disps && i < old.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        if (!std::isfinite(old.disps[i][axis])) {
          return false;
        }
      }
    }
    for (int i = 0; i < grid.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        const float delta = grid.disps[i][axis] - base[corner].disps[i][axis];
        if (!std::isfinite(delta)) {
          return false;
        }
        grid.disps[i][axis] = delta;
      }
    }
    /* The scratch surface carries the BASE hidden bitmap. Replace it with the
     * layer's own bitmap, using exactly the old resample_all mapping. */
    MEM_SAFE_DELETE(grid.hidden);
    if (old.hidden) {
      grid.hidden = BLI_BITMAP_NEW(grid.totdisp, "subdivided sculpt layer hidden");
      for (int y = 0; y < new_side; y++) {
        const float v = float(y) * float(old_side - 1) / float(new_side - 1);
        const int sy = int(std::round(v));
        for (int x = 0; x < new_side; x++) {
          const float u = float(x) * float(old_side - 1) / float(new_side - 1);
          const int sx = int(std::round(u));
          BLI_BITMAP_SET(grid.hidden, y * new_side + x,
                         BLI_BITMAP_TEST_BOOL(old.hidden, sy * old_side + sx));
        }
      }
    }
  }
  return true;
}

static bool multires_stage_subdivided_layer(const MultiresSculptLayer &layer,
                                           const MDisps *base,
                                           MDisps *result,
                                           const int grids_num,
                                           const int new_level,
                                           const bool allow_sparse = false)
{
  if (!multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_PARALLEL_V01")) {
    return multires_stage_subdivided_layer_serial_v01(
        layer, base, result, grids_num, new_level, allow_sparse);
  }

  if (!base || !result || !layer.disps || layer.grids_num != grids_num ||
      layer.level < 1 || new_level <= layer.level || new_level > 13)
  {
    return false;
  }
  const int old_side = bke::subdiv::grid_size_from_level(layer.level);
  const int new_side = bke::subdiv::grid_size_from_level(new_level);
  const auto validate = [&](const int corner) {
    const MDisps &old = layer.disps[corner];
    const MDisps &grid = result[corner];
    if ((!old.disps && !allow_sparse) || old.level != layer.level || old.totdisp != old_side * old_side ||
        !base[corner].disps || base[corner].level != new_level ||
        base[corner].totdisp != new_side * new_side || !grid.disps ||
        grid.level != new_level || grid.totdisp != new_side * new_side)
    {
      return false;
    }
    /* Retain resample_all's validation of the old layer, including hidden layers. */
    for (int i = 0; old.disps && i < old.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        if (!std::isfinite(old.disps[i][axis])) {
          return false;
        }
      }
    }
    return true;
  };
  std::atomic<bool> valid{true};
  {
    MultiresSubdivisionPhaseTimer timer("validate_delta_v01", "layer", new_level, grids_num);
    multires_subdivide_grids_v01(grids_num, new_side * new_side, [&](const int corner) {
      if (!validate(corner)) {
        valid.store(false, std::memory_order_relaxed);
      }
    });
  }
  if (!valid.load(std::memory_order_relaxed)) {
    return false;
  }
  multires_subdivide_grids_v01(grids_num, new_side * new_side, [&](const int corner) {
    const MDisps &old = layer.disps[corner];
    MDisps &grid = result[corner];
    for (int i = 0; i < grid.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        const float delta = grid.disps[i][axis] - base[corner].disps[i][axis];
        if (!std::isfinite(delta)) {
          valid.store(false, std::memory_order_relaxed);
        }
        grid.disps[i][axis] = delta;
      }
    }
    /* The scratch surface carries the BASE hidden bitmap. Replace it with the
     * layer's own bitmap, using exactly the old resample_all mapping. */
    MEM_SAFE_DELETE(grid.hidden);
    if (old.hidden) {
      grid.hidden = BLI_BITMAP_NEW(grid.totdisp, "subdivided sculpt layer hidden");
      for (int y = 0; y < new_side; y++) {
        const float v = float(y) * float(old_side - 1) / float(new_side - 1);
        const int sy = int(std::round(v));
        for (int x = 0; x < new_side; x++) {
          const float u = float(x) * float(old_side - 1) / float(new_side - 1);
          const int sx = int(std::round(u));
          BLI_BITMAP_SET(grid.hidden, y * new_side + x,
                         BLI_BITMAP_TEST_BOOL(old.hidden, sy * old_side + sx));
        }
      }
    }
  });
  if (BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PROFILE")) {
    std::printf("[SUBDIVIDE_WORK_V01] grids=%d area=%d lanes=%d valid=%d\n",
                grids_num, new_side * new_side,
                multires_subdivide_lanes_v01(grids_num, new_side * new_side),
                int(valid.load(std::memory_order_relaxed)));
  }
  return valid.load(std::memory_order_relaxed);
}

/* Subdivision propagates detail beyond the old support. Never skip old null
 * grids locally: run the native global surface algorithm first, and release
 * only final all-zero delta arrays after successful extraction. */
static void multires_subdivide_prune_zero_v01(MDisps *grids,
                                               const int count,
                                               const int level)
{
  if (!multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_ZERO_V01")) {
    return;
  }
  MultiresSubdivisionPhaseTimer timer("prune_exact_zero_v01", "layer", level, count);
  std::atomic<int64_t> released{0};
  const int side = bke::subdiv::grid_size_from_level(level);
  multires_subdivide_grids_v01(count, side * side, [&](const int grid) {
    if (bke::multires_layers::subdivide_work_v01::exactly_zero(
            grids[grid].disps, grids[grid].totdisp)) {
      MEM_SAFE_DELETE(grids[grid].disps);
      /* Preserve the descriptor and its independent hidden bitmap. */
      released.fetch_add(1, std::memory_order_relaxed);
    }
  });
  if (BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PROFILE")) {
    std::printf("[SUBDIVIDE_ZERO_V01] grids=%d released=%lld payload_saved_mib=%.3f\n",
                count, static_cast<long long>(released.load()),
                double(released.load()) * side * side * sizeof(float[3]) / (1024.0 * 1024.0));
  }
}

/* Called only after every layer has been staged and the sculpt session flushed.
 * Scratch destruction releases the old grids after ownership is exchanged. */
static void multires_publish_subdivided_layer(MultiresSculptLayer &layer,
                                             MDisps *result,
                                             const int new_level)
{
  for (int corner = 0; corner < layer.grids_num; corner++) {
    std::swap(layer.disps[corner], result[corner]);
  }
  layer.level = new_level;
}

/* The first sparse-subdivide optimization handles structurally empty layers
 * only. Partial layers keep the existing global algorithm and seam propagation.
 * L0 contributions and layer hidden bitmaps retain their established path. */
static bool multires_layer_can_keep_empty_subdivide(const MultiresSculptLayer &layer)
{
  if (layer.level <= 0 || layer.level > 13 || layer.disps == nullptr || layer.grids_num <= 0) {
    return false;
  }
  if (layer.base_deltas != nullptr) {
    for (int vertex = 0; vertex < layer.base_verts_num; vertex++) {
      const float *delta = layer.base_deltas[vertex];
      if (delta[0] != 0.0f || delta[1] != 0.0f || delta[2] != 0.0f) {
        return false;
      }
    }
  }
  const int side = bke::subdiv::grid_size_from_level(layer.level);
  for (int grid = 0; grid < layer.grids_num; grid++) {
    const MDisps &data = layer.disps[grid];
    if (data.disps != nullptr || data.hidden != nullptr || data.level != layer.level ||
        data.totdisp != side * side)
    {
      return false;
    }
  }
  return true;
}

static void multires_publish_empty_subdivided_layer(MultiresSculptLayer &layer,
                                                   const int new_level)
{
  const int side = bke::subdiv::grid_size_from_level(new_level);
  for (int grid = 0; grid < layer.grids_num; grid++) {
    BLI_assert(layer.disps[grid].disps == nullptr && layer.disps[grid].hidden == nullptr);
    layer.disps[grid].level = new_level;
    layer.disps[grid].totdisp = side * side;
  }
  layer.level = new_level;
}

/* Check allocated zero buffers too. Hidden grids and nonzero L0 deltas retain
 * the established surface pipeline. A non-finite component cannot be empty. */
static bool multires_compact_layer_is_empty(const MultiresSculptLayer &layer)
{
  if (layer.level < 1 || layer.level > 13 || !layer.disps || layer.grids_num <= 0) {
    return false;
  }
  if (layer.base_deltas &&
      !bke::multires_layers::subdivide_work_v01::exactly_zero(
          layer.base_deltas, layer.base_verts_num)) {
    return false;
  }
  const int side = bke::subdiv::grid_size_from_level(layer.level);
  for (int corner = 0; corner < layer.grids_num; corner++) {
    const MDisps &grid = layer.disps[corner];
    if (grid.level != layer.level || grid.totdisp != side * side || grid.hidden) {
      return false;
    }
    if (grid.disps &&
        !bke::multires_layers::subdivide_work_v01::exactly_zero(grid.disps, grid.totdisp)) {
      return false;
    }
  }
  return true;
}

/* All new payloads remain staged until every layer succeeds. Retain only the
 * necessary MDisps, rather than a complete Mesh and paint masks per layer. */
struct MultiresCompactSubdivisionStage {
  int corners_num;
  std::vector<std::pair<MultiresSculptLayer *, MDisps *>> layers;

  explicit MultiresCompactSubdivisionStage(const int corners_num) : corners_num(corners_num) {}
  MultiresCompactSubdivisionStage(const MultiresCompactSubdivisionStage &) = delete;
  MultiresCompactSubdivisionStage &operator=(const MultiresCompactSubdivisionStage &) = delete;
  ~MultiresCompactSubdivisionStage()
  {
    for (const auto &[layer, grids] : layers) {
      (void)layer;
      for (int corner = 0; corner < corners_num; corner++) {
        MEM_SAFE_DELETE(grids[corner].disps);
        MEM_SAFE_DELETE(grids[corner].hidden);
      }
      MEM_delete(grids);
    }
  }
};

static bool multires_subdivide_compact_layers(Object *object,
                                              MultiresModifierData *mmd,
                                              const int top_level,
                                              const MultiresSubdivideModeType mode)
{
  const double started = BLI_time_now_seconds();
  Mesh *mesh = id_cast<Mesh *>(object->data);
  MultiresSubdivisionPhaseTimer total_timer("compact_including_cleanup", "all", top_level, mesh->corners_num);
  std::vector<MultiresSculptLayer *> empty;
  empty.reserve(mmd->sculpt_layers.count());
  {
  MultiresSubdivisionPhaseTimer timer("classify_empty", "all", top_level, mesh->corners_num);
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (multires_compact_layer_is_empty(layer)) {
      empty.push_back(&layer);
    }
  }
  }
  MultiresSubdivideCoarseV02 coarse;
  Mesh *base = multires_subdivide_layer_surface(object, *mmd, nullptr, top_level, mode, &coarse);
  BLI_SCOPED_DEFER([&]() { if (base) { BKE_id_free(nullptr, base); } });
  MultiresCompactSubdivisionStage staged(mesh->corners_num);
  staged.layers.reserve(mmd->sculpt_layers.count());
  bool valid = base != nullptr;
  const MDisps *base_grids = base ? static_cast<const MDisps *>(
      CustomData_get_layer(&base->corner_data, CD_MDISPS)) : nullptr;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (!valid) {
      break;
    }
    if (std::find(empty.begin(), empty.end(), &layer) != empty.end()) {
      continue;
    }
    Array<int8_t> regional_grids(
        multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_REGIONAL_V03") ? mesh->corners_num : 0,
        int8_t(1));
    Mesh *surface = multires_subdivide_layer_surface(
        object, *mmd, &layer, top_level, mode, &coarse, regional_grids);
    if (!surface) {
      valid = false;
      break;
    }
    MDisps *grids = static_cast<MDisps *>(CustomData_get_layer_for_write(
        &surface->corner_data, CD_MDISPS, surface->corners_num));
    if (!regional_grids.is_empty()) {
      /* Exact base bytes produce exact zero deltas in unchanged native staging. */
      const int side = bke::subdiv::grid_size_from_level(top_level);
      for (int g = 0; g < mesh->corners_num; ++g) {
        if (!regional_grids[g]) {
          if (!grids[g].disps || !base_grids[g].disps ||
              grids[g].level != top_level || base_grids[g].level != top_level ||
              grids[g].totdisp != side * side || base_grids[g].totdisp != side * side)
          {
            valid = false;
            break;
          }
          std::memcpy(grids[g].disps, base_grids[g].disps,
                      sizeof(float[3]) * grids[g].totdisp);
        }
      }
    }
    if (!valid) {
      BKE_id_free(nullptr, surface);
      break;
    }
    {
    MultiresSubdivisionPhaseTimer timer("extract_delta", "layer", top_level, mesh->corners_num);
    valid = multires_stage_subdivided_layer(
        layer, base_grids, grids, mesh->corners_num, top_level, true);
    }
    if (valid) {
      multires_subdivide_prune_zero_v01(grids, mesh->corners_num, top_level);
      MDisps *owned = MEM_new_array_zeroed<MDisps>(mesh->corners_num, "staged compact layer grids");
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        std::swap(owned[corner], grids[corner]);
      }
      staged.layers.emplace_back(&layer, owned);
    }
    /* The layer's final grids are owned by staged; discard its duplicate mesh,
     * attributes and paint-mask storage before processing the next layer. */
    {
    MultiresSubdivisionPhaseTimer timer("free_surface_mesh", "layer", top_level, mesh->corners_num);
    BKE_id_free(nullptr, surface);
    }
  }
  {
  MultiresSubdivisionPhaseTimer timer("force_sculpt_rebuild", "all", top_level, mesh->corners_num);
  valid = valid && multires_force_sculpt_rebuild(object);
  }
  if (valid) {
    MultiresSubdivisionPhaseTimer timer("publish_facesets", "all", top_level, mesh->corners_num);
    for (MultiresSculptLayer *layer : empty) {
      bke::multires_layers::weight_hot_delta_cache_invalidate(layer);
      for (int corner = 0; corner < layer->grids_num; corner++) {
        MEM_SAFE_DELETE(layer->disps[corner].disps);
      }
      multires_publish_empty_subdivided_layer(*layer, top_level);
    }
    for (const auto &[layer, grids] : staged.layers) {
      bke::multires_layers::weight_hot_delta_cache_invalidate(layer);
      multires_publish_subdivided_layer(*layer, grids, top_level);
    }
    MDisps *destination = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));
    MDisps *new_base = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&base->corner_data, CD_MDISPS, mesh->corners_num));
    for (int corner = 0; corner < mesh->corners_num; corner++) {
      std::swap(destination[corner], new_base[corner]);
    }
    GridPaintMask *masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &mesh->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    GridPaintMask *new_masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &base->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    if (masks && new_masks) {
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        std::swap(masks[corner], new_masks[corner]);
      }
    }
    multires_set_tot_level(object, mmd, top_level);
    if (mmd->sculpt_face_set_grids) {
      bke::multires_layers::faceset_resample_up(*mmd, top_level);
    }
    mmd->sculpt_layer_recording = 0;
  }
  if (BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PROFILE")) {
    std::printf("[SUBDIVIDE_COMPACT] success=%d layers=%d empty=%d active=%d live_layer_meshes_max=1 total=%.3fms\n",
                int(valid), int(mmd->sculpt_layers.count()), int(empty.size()),
                int(staged.layers.size()), (BLI_time_now_seconds() - started) * 1000.0);
  }
  return valid;
}

void multiresModifier_subdivide_to_level(Object *object,
                                         MultiresModifierData *mmd,
                                         const int top_level,
                                         const MultiresSubdivideModeType mode)
{
  if (top_level <= mmd->totlvl) {
    return;
  }
  Mesh *mesh = id_cast<Mesh *>(object->data);
  MultiresSubdivisionPhaseTimer all_timer("subdivide_total", "all", top_level, mesh->corners_num);

  /* Capture Face Sets before subdivision. From this point onward the CCG-grid
   * master survives independently of the coarse face-domain attribute. */
  if (mmd->totlvl > 0 && mesh->attributes().contains(".sculpt_face_set")) {
    if (!bke::multires_layers::faceset_ensure_from_mesh(*mmd, *mesh, mmd->totlvl)) {
      BKE_modifier_set_error(object, &mmd->modifier,
                             "Cannot initialize high-resolution Multires Face Sets");
      return;
    }
  }

  if (mmd->totlvl == 0 && !mmd->sculpt_layers.is_empty()) {
    if (!bke::multires_layers::base_remap_after_topology_edit(*mmd, *mesh) ||
        !bke::multires_layers::commit(*object)) {
      BKE_modifier_set_error(object, &mmd->modifier, "Cannot preserve L0 layers during subdivision");
      return;
    }
    multiresModifier_subdivide_to_level_base(object, mmd, top_level, mode);
    if (mmd->totlvl == top_level) {
      bke::multires_layers::initialize_layer_grids(*mmd, top_level, mesh->corners_num);
      bke::multires_layers::ensure_corner_ids(*mmd, *mesh);
      if (!bke::multires_layers::bind_initialized_corner_grids(*mmd, *mesh)) {
        BKE_modifier_set_error(object, &mmd->modifier,
                               "Could not bind initialized Sculpt Layer grids");
      }
    }
    return;
  }
  if (mmd->sculpt_layers.is_empty()) {
    multiresModifier_subdivide_to_level_base(object, mmd, top_level, mode);
    if (mmd->sculpt_face_set_grids && mmd->totlvl >= top_level) {
      bke::multires_layers::faceset_resample_up(*mmd, top_level);
    }
    return;
  }
  if (!bke::multires_layers::remap_after_topology_edit(*mmd, *mesh)) {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Cannot preserve Sculpt Layers: invalid corner storage binding; legacy non-canonical IDs require an Edit round-trip");
    return;
  }
  if (top_level > 13 || ID_REAL_USERS(&mesh->id) > 1 ||
      !bke::multires_layers::compatible(*mmd, *mesh) ||
      !bke::multires_layers::commit(*object))
  {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Cannot subdivide: sculpt layer grids or pending edits are invalid");
    return;
  }

  const char *compact_env = BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_COMPACT");
  if (compact_env && compact_env[0] == '1' && compact_env[1] == '\0') {
    if (!multires_subdivide_compact_layers(object, mmd, top_level, mode)) {
      BKE_modifier_set_error(object, &mmd->modifier,
                             "Compact Sculpt Layer subdivision failed; original grids retained");
    }
    return;
  }

  /* V4.5c sparse subdivision safety.
   *
   * Sparse Sculpt Layer grids use nullptr as an implicit all-zero payload.
   * The legacy subdivision pipeline is global, so materialize only existing
   * positive-level layer grids here. A level-zero layer legitimately has no
   * grid payload yet: its base_deltas are carried by the cage and the null
   * high-resolution contribution must remain equivalent to zero. */
  const char *sparse_env = BLI_getenv("BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE");
  const bool keep_empty = sparse_env != nullptr && sparse_env[0] == '1' && sparse_env[1] == '\0';
  std::vector<MultiresSculptLayer *> empty_layers;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (keep_empty && multires_layer_can_keep_empty_subdivide(layer)) {
      empty_layers.push_back(&layer);
      continue;
    }
    if (layer.level <= 0) {
      continue;
    }
    if (!layer.disps || layer.grids_num <= 0) {
      BKE_modifier_set_error(
          object, &mmd->modifier, "Cannot subdivide: invalid sparse Sculpt Layer descriptors");
      return;
    }
    for (int corner = 0; corner < layer.grids_num; corner++) {
      if (bke::multires_layers::grid_ensure(layer, corner) == nullptr) {
        BKE_modifier_set_error(
            object, &mmd->modifier, "Cannot subdivide: failed to materialize Sculpt Layer grid");
        return;
      }
    }
  }

  Mesh *base = multires_subdivide_layer_surface(object, *mmd, nullptr, top_level, mode);
  std::vector<std::pair<MultiresSculptLayer *, Mesh *>> surfaces;
  bool valid = base != nullptr;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (std::find(empty_layers.begin(), empty_layers.end(), &layer) != empty_layers.end()) {
      continue;
    }
    if (!valid) {
      break;
    }
    Mesh *surface = multires_subdivide_layer_surface(object, *mmd, &layer, top_level, mode);
    surfaces.emplace_back(&layer, surface);
    valid = surface != nullptr;
  }
  const MDisps *base_grids = base ? static_cast<const MDisps *>(
      CustomData_get_layer(&base->corner_data, CD_MDISPS)) : nullptr;
  for (const auto &[layer, surface] : surfaces) {
    if (!valid) {
      break;
    }
    MDisps *grids = static_cast<MDisps *>(CustomData_get_layer_for_write(
        &surface->corner_data, CD_MDISPS, surface->corners_num));
    valid = multires_stage_subdivided_layer(
        *layer, base_grids, grids, mesh->corners_num, top_level);
  }
  valid = valid && multires_force_sculpt_rebuild(object);
  if (valid) {
    for (MultiresSculptLayer *layer : empty_layers) {
      bke::multires_layers::weight_hot_delta_cache_invalidate(layer);
      multires_publish_empty_subdivided_layer(*layer, top_level);
    }
    if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
      std::printf("[LAYER_EMPTY_SUBDIVIDE] skipped=%d grids=%d old_level=%d new_level=%d\n",
                  int(empty_layers.size()), mesh->corners_num, mmd->totlvl, top_level);
    }
    for (const auto &[layer, surface] : surfaces) {
      MDisps *grids = static_cast<MDisps *>(CustomData_get_layer_for_write(
          &surface->corner_data, CD_MDISPS, surface->corners_num));
      multires_publish_subdivided_layer(*layer, grids, top_level);
    }
    MDisps *destination = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));
    MDisps *new_base = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&base->corner_data, CD_MDISPS, mesh->corners_num));
    for (int corner = 0; corner < mesh->corners_num; corner++) {
      std::swap(destination[corner], new_base[corner]);
    }
    GridPaintMask *masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &mesh->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    GridPaintMask *new_masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &base->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    if (masks && new_masks) {
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        std::swap(masks[corner], new_masks[corner]);
      }
    }
    multires_set_tot_level(object, mmd, top_level);
    if (mmd->sculpt_face_set_grids) {
      bke::multires_layers::faceset_resample_up(*mmd, top_level);
    }
    mmd->sculpt_layer_recording = 0;
  }
  else {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Sculpt Layer subdivision failed; original grids retained");
  }
  if (base) {
    BKE_id_free(nullptr, base);
  }
  for (const auto &[layer, surface] : surfaces) {
    if (surface) {
      BKE_id_free(nullptr, surface);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply base
 * \{ */

static bke::multires_layers::conform::Frame multires_conform_frame(
    const MultiresReshapeContext &context, const GridCoord &coord)
{
  float3 position;
  float3x3 tangent;
  multires_reshape_evaluate_base_mesh_limit_at_grid(&context, &coord, position, tangent);
  bke::multires_layers::conform::Frame frame{};
  for (int r = 0; r < 3; r++) {
    frame.origin[r] = position[r];
    for (int c = 0; c < 3; c++) {
      frame.axes[r][c] = tangent[c][r];
    }
  }
  return frame;
}

const char *multiresModifier_conform_layers(Object *object,
                                           MultiresModifierData *mmd,
                                           const bool include_visible_layers)
{
  printf("[CONFORM_TRACE] CORE ENTER layers=%d\n", BLI_listbase_count(&mmd->sculpt_layers));
  fflush(stdout);
  using namespace bke::multires_layers::conform;
  if (!object || !mmd || object->mode != OB_MODE_OBJECT) {
    return "Layer-aware Conform currently requires Object Mode";
  }
  Mesh *mesh = id_cast<Mesh *>(object->data);
  if (mesh->key || ID_REAL_USERS(&mesh->id) > 1 ||
      CustomData_external_test(&mesh->corner_data, CD_MDISPS))
  {
    return "Conform requires a single-user mesh without shape keys or external displacement";
  }
  if (object->modifiers.count() != 1 || object->modifiers.first != &mmd->modifier) {
    return "Layer-aware Conform currently supports a single Multires modifier only";
  }
  /*
   * Sculpt Layer compatibility is required only when a layer stack exists.
   * With an empty stack this routine also acts as the standard Multires
   * Conform path: conform the cage and rebase the base MDisps only.
   */
  if (!mmd->sculpt_layers.is_empty() &&
      !bke::multires_layers::compatible(*mmd, *mesh))
  {
    return "Conform requires valid Sculpt Layers at the stored Multires level";
  }
  /* Object Mode has no pending brush transaction. Never publish a cage while
   * a live sculpt session owns uncommitted grid coordinates. */
  if (object->runtime->sculpt_session) {
    return "Finish the sculpt session before using layer-aware Conform";
  }

  MultiresModifierData evaluator_mmd = dna::shallow_copy(*mmd);
  evaluator_mmd.sculpt_layers = {};
  MultiresReshapeContext old_context;
  if (!multires_reshape_context_create_from_modifier(
          &old_context, object, &evaluator_mmd, mmd->totlvl))
  {
    return "Could not evaluate the original Multires surface";
  }
  BLI_SCOPED_DEFER([&]() { multires_reshape_context_free(&old_context); });

  Mesh *work = BKE_mesh_copy_for_eval(*mesh);
  BLI_SCOPED_DEFER([&]() { BKE_id_free(nullptr, work); });
  const MDisps *base = static_cast<const MDisps *>(
      CustomData_get_layer(&mesh->corner_data, CD_MDISPS));
  if (!base) {
    return "Conform requires valid base Multires displacement grids";
  }
  const int side = bke::subdiv::grid_size_from_level(mmd->totlvl);
  const int area = side * side;
  /*
   * CONFORM_HYBRID_RESTORE_FINAL_V1
   *
   * "Conform from Final Result" must actually include visible weighted
   * Sculpt Layers when choosing the new cage. This is the original
   * layer-aware behavior.
   *
   * Without Layers / standard Conform keeps Blender's native cage core.
   */
  if (include_visible_layers && !mmd->sculpt_layers.is_empty()) {
    Array<float3> sums(mesh->verts_num, float3(0.0f));
    Array<int> counts(mesh->verts_num, 0);
    const Span<int> corner_verts = mesh->corner_verts();

    for (const int corner : corner_verts.index_range()) {
      const GridCoord coord{corner, 1.0f, 1.0f};
      const Frame frame = multires_conform_frame(old_context, coord);

      if (base[corner].disps == nullptr || base[corner].totdisp != area) {
        return "Conform requires materialized base Multires grids";
      }

      double displacement[3];
      for (int axis = 0; axis < 3; axis++) {
        displacement[axis] = base[corner].disps[area - 1][axis];
      }

      for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
        if (!(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
            layer.strength == 0.0f ||
            layer.disps == nullptr)
        {
          continue;
        }

        const MDisps &layer_grid = layer.disps[corner];
        if (layer_grid.disps == nullptr) {
          continue;
        }

        for (int axis = 0; axis < 3; axis++) {
          displacement[axis] +=
              double(layer.strength) * layer_grid.disps[area - 1][axis];
        }
      }

      float3 position;
      for (int r = 0; r < 3; r++) {
        double value = frame.origin[r];

        for (int axis = 0; axis < 3; axis++) {
          value += frame.axes[r][axis] * displacement[axis];
        }

        if (!std::isfinite(value) ||
            std::abs(value) > std::numeric_limits<float>::max())
        {
          return "Non-finite cage coordinates; original data retained";
        }

        position[r] = float(value);
      }

      const int vert = corner_verts[corner];
      sums[vert] += position;
      counts[vert]++;
    }

    MutableSpan<float3> positions = work->vert_positions_for_write();

    for (const int vert : positions.index_range()) {
      if (counts[vert] == 0) {
        continue;
      }

      positions[vert] = sums[vert] / float(counts[vert]);

      for (int axis = 0; axis < 3; axis++) {
        if (!std::isfinite(positions[vert][axis])) {
          return "Non-finite conformed cage; original data retained";
        }
      }
    }

    work->tag_positions_changed();
  }
  else {
    /*
     * Base-only target: use Blender's native Conform Base cage fitting.
     * evaluator_mmd deliberately contains no Sculpt Layers.
     */
    Object work_object = dna::shallow_copy(*object);
    work_object.data = &work->id;

    MultiresReshapeContext cage_context;
    if (!multires_reshape_context_create_from_modifier(
            &cage_context, &work_object, &evaluator_mmd, mmd->totlvl))
    {
      return "Could not initialize native Conform Base cage; original data retained";
    }

    BLI_SCOPED_DEFER([&]() {
      multires_reshape_context_free(&cage_context);
    });

    multires_reshape_store_original_grids(&cage_context);
    multires_reshape_assign_final_coords_from_mdisps(&cage_context);
    multires_reshape_apply_base_refine_from_base(&cage_context);
    multires_reshape_apply_base_update_mesh_coords(&cage_context);

    work->tag_positions_changed();
  }

  Object work_object = dna::shallow_copy(*object);
  work_object.data = &work->id;
  MultiresReshapeContext new_context;
  if (!multires_reshape_context_create_from_modifier(
          &new_context, &work_object, &evaluator_mmd, mmd->totlvl))
  {
    return "Could not evaluate the conformed cage; original data retained";
  }
  BLI_SCOPED_DEFER([&]() { multires_reshape_context_free(&new_context); });

  MultiresModifierData staged = dna::shallow_copy(*mmd);
  staged.sculpt_layers = {};
  bke::multires_layers::copy(staged, *mmd);
  BLI_SCOPED_DEFER([&]() { bke::multires_layers::clear(staged); });
  std::vector<std::pair<MultiresSculptLayer *, MultiresSculptLayer *>> layers;
  auto *dst_layer = static_cast<MultiresSculptLayer *>(staged.sculpt_layers.first);
  for (MultiresSculptLayer &src_layer : mmd->sculpt_layers) {
    layers.emplace_back(&src_layer, dst_layer);
    dst_layer = dst_layer->next;
  }
  MDisps *rebased = static_cast<MDisps *>(CustomData_get_layer_for_write(
      &work->corner_data, CD_MDISPS, work->corners_num));
  double conform_base_delta_sum = 0.0;
  double conform_layer_delta_sum = 0.0;
  double conform_base_delta_max = 0.0;
  double conform_layer_delta_max = 0.0;
  uint64_t conform_base_samples = 0;
  uint64_t conform_layer_samples = 0;

  for (int corner = 0; corner < mesh->corners_num; corner++) {
    for (int y = 0; y < side; y++) {
      for (int x = 0; x < side; x++) {
        const int i = y * side + x;
        const GridCoord coord{corner, float(x) / (side - 1), float(y) / (side - 1)};
        const Frame before = multires_conform_frame(old_context, coord);
        const Frame after = multires_conform_frame(new_context, coord);
        Mapping mapping;
        if (!make_mapping(before, after, mapping)) {
          return "Degenerate tangent frame; original data retained";
        }
        if (!rebase(mapping, base[corner].disps[i], true, rebased[corner].disps[i]) ||
            !preserves(before, after, base[corner].disps[i], rebased[corner].disps[i], true))
        {
          return "Base preservation check failed; original data retained";
        }
        for (int axis = 0; axis < 3; axis++) {
          const double d = double(rebased[corner].disps[i][axis]) -
                           double(base[corner].disps[i][axis]);
          conform_base_delta_sum += d * d;
          conform_base_delta_max = std::max(conform_base_delta_max, std::abs(d));
          conform_base_samples++;
        }

        for (const auto &[src, dst] : layers) {
          const MDisps &src_grid = src->disps[corner];
          MDisps &dst_grid = dst->disps[corner];
          /* Sparse Sculpt Layer grids use nullptr as an implicit all-zero delta.
           * Rebasing a zero vector is still zero, so preserve sparsity instead of
           * dereferencing or materializing the grid. */
          if (src_grid.disps == nullptr) {
            continue;
          }
          if (dst_grid.disps == nullptr ||
              !rebase(mapping, src_grid.disps[i], false, dst_grid.disps[i]) ||
              !preserves(before, after, src_grid.disps[i], dst_grid.disps[i], false))
          {
            return "Layer preservation check failed; original data retained";
          }

          for (int axis = 0; axis < 3; axis++) {
            const double d = double(dst_grid.disps[i][axis]) -
                             double(src_grid.disps[i][axis]);
            conform_layer_delta_sum += d * d;
            conform_layer_delta_max = std::max(conform_layer_delta_max, std::abs(d));
            conform_layer_samples++;
          }
        }
        float old_final[3], new_final[3];
        for (int axis = 0; axis < 3; axis++) {
          double old_value = base[corner].disps[i][axis];
          double new_value = rebased[corner].disps[i][axis];
          for (const auto &[src, dst] : layers) {
            if (src->flag & MULTIRES_SCULPT_LAYER_VISIBLE) {
              const MDisps &src_grid = src->disps[corner];
              const MDisps &dst_grid = dst->disps[corner];
              if (src_grid.disps != nullptr) {
                if (dst_grid.disps == nullptr) {
                  return "Layer preservation check failed; original data retained";
                }
                old_value += double(src->strength) * src_grid.disps[i][axis];
                new_value += double(src->strength) * dst_grid.disps[i][axis];
              }
            }
          }
          if (!std::isfinite(old_value) || !std::isfinite(new_value) ||
              std::abs(old_value) > std::numeric_limits<float>::max() ||
              std::abs(new_value) > std::numeric_limits<float>::max())
          {
            return "Non-finite composed surface; original data retained";
          }
          old_final[axis] = float(old_value);
          new_final[axis] = float(new_value);
        }
        if (!preserves(before, after, old_final, new_final, true)) {
          return "Final surface preservation check failed; original data retained";
        }
      }
    }
  }
  printf(
      "[CONFORM_MEASURE] base_rms=%g base_max=%g base_n=%llu "
      "layer_rms=%g layer_max=%g layer_n=%llu\n",
      conform_base_samples ?
          std::sqrt(conform_base_delta_sum / double(conform_base_samples)) : 0.0,
      conform_base_delta_max,
      (unsigned long long)conform_base_samples,
      conform_layer_samples ?
          std::sqrt(conform_layer_delta_sum / double(conform_layer_samples)) : 0.0,
      conform_layer_delta_max,
      (unsigned long long)conform_layer_samples);
  fflush(stdout);

  printf("[CONFORM_TRACE] VALIDATION PASSED\n");
  fflush(stdout);

  /* All conversion and validation completed on owned scratch storage. Preserve
   * layer objects, names, visibility, strength and RNA animation paths. */
  /* LAYER_CCG_UNDO_CACHE_GUARD_V1: invalidate derived layer payload caches
   * before publishing the rebased buffers. Never flush an old Sculpt CCG here. */
  bke::multires_layers::weight_hot_delta_cache_invalidate();
  MDisps *destination = static_cast<MDisps *>(CustomData_get_layer_for_write(
      &mesh->corner_data, CD_MDISPS, mesh->corners_num));
  mesh->vert_positions_for_write().copy_from(work->vert_positions());


  for (int corner = 0; corner < mesh->corners_num; corner++) {
    std::swap(destination[corner], rebased[corner]);
  }
  for (const auto &[src, dst] : layers) {
    std::swap(src->disps, dst->disps);
  }
  mmd->sculpt_layer_recording = 0;
  mesh->tag_positions_changed();
  printf("[CONFORM_TRACE] PUBLISH COMPLETE\n");
  fflush(stdout);
  return nullptr;
}

void multiresModifier_base_apply(Depsgraph *depsgraph,
                                 Object *object,
                                 MultiresModifierData *mmd,
                                 const ApplyBaseMode mode)
{
  multires_force_sculpt_rebuild(object);

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(&reshape_context, depsgraph, object, mmd)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);

  /* At this point base_mesh is object's mesh, the subdiv is initialized to the deformed state of
   * the base mesh.
   * Store coordinates of top level grids in object space which will define true shape we would
   * want to reshape to after modifying the base mesh. */
  multires_reshape_assign_final_coords_from_mdisps(&reshape_context);

  /* For modifying base mesh we only want to consider deformation caused by multires displacement
   * and ignore all deformation which might be caused by deformation modifiers leading the multires
   * one.
   * So refine the subdiv to the original mesh vertices positions, which will also need to make
   * it so object space displacement is re-evaluated for them (as in, can not re-use any knowledge
   * from the final coordinates in the object space ). */
  multires_reshape_apply_base_refine_from_base(&reshape_context);

  /* Modify original mesh coordinates. This happens in two steps:
   * - Coordinates are set to their final location, where they are intended to be in the final
   *   result.
   * - Heuristic moves them a bit, kind of canceling out the effect of subsurf (so then when
   *   multires modifier applies subsurf vertices are placed at the desired location). */
  multires_reshape_apply_base_update_mesh_coords(&reshape_context);
  if (mode == ApplyBaseMode::ForSubdivision) {
    multires_reshape_apply_base_refit_base_mesh(&reshape_context);
  }
  multires_reshape_apply_base_update_shape_key(&reshape_context);

  /* Reshape to the stored final state.
   * Not that the base changed, so the subdiv is to be refined to the new positions. Unfortunately,
   * this can not be done foe entirely cheap: if there were deformation modifiers prior to the
   * multires they need to be re-evaluated for the new base mesh. */
  multires_reshape_apply_base_refine_from_deform(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);

  multires_reshape_context_free(&reshape_context);
}

/** \} */

}  // namespace blender
