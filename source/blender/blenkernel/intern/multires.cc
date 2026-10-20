/* SPDX-FileCopyrightText: 2007 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include <cstdio>

/* for reading old multires */
#define DNA_DEPRECATED_ALLOW

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "BLI_bitmap.h"
#include "BLI_index_mask.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_vector_set.hh"
#include "BLI_vector.hh"
#include "BLI_memory_utils.hh"
#include "BKE_lib_id.hh"

#include "BKE_ccg.hh"
#include "BKE_editmesh.hh"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.hh"
#include "BKE_mesh_types.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_sculpt_cold_start_profile.hh"
#include "multires_layer_cache_budget.hh"
#include "multires_weight_multi_cache_v01.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_sculpt_facesets_visibility_rc2.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_scene.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_eval.hh"
#include "BKE_texture.h"
#include "multires_reshape.hh"

#include "BKE_object.hh"
#include "BKE_action.hh"
#include "DNA_action_types.h"
#include "BLI_math_matrix.hh"

#include "DEG_depsgraph_query.hh"

#include "RE_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <cstdlib>
#include <cstdint>
#include <mutex>

namespace blender {

float4x4 multires_texture_mapping_matrix(const MultiresModifierData &mmd,
                                        const Object &object,
                                        const Object *map_object)
{
  if (mmd.sculpt_displacement_mapping == 2) {
    return object.object_to_world();
  }
  if (mmd.sculpt_displacement_mapping == 3) {
    const Object *reference = map_object ? map_object : mmd.sculpt_displacement_map_object;
    if (reference) {
      float4x4 reference_world = reference->object_to_world();
      if (mmd.sculpt_displacement_map_bone[0]) {
        const bPoseChannel *pchan = BKE_pose_channel_find_name(
            reference->pose, mmd.sculpt_displacement_map_bone);
        if (pchan) {
          reference_world = reference_world * float4x4(pchan->pose_mat);
        }
      }
      return math::invert(reference_world) * object.object_to_world();
    }
  }
  return float4x4::identity();
}

float multires_texture_displacement_sample(const Mesh &mesh,
                                           bke::subdiv::Subdiv &subdiv,
                                           const MultiresModifierData &mmd,
                                           const int ptex_face_index,
                                           const float u,
                                           const float v,
                                           const float4x4 *mapping_matrix)
{
  if (mmd.sculpt_displacement_enabled == 0 || mmd.sculpt_displacement_texture == nullptr ||
      mmd.sculpt_displacement_strength == 0.0f)
  {
    return 0.0f;
  }

  float3 tex_co;
  if (mmd.sculpt_displacement_mapping == 0) {
    const VectorSet<StringRefNull> uv_names = mesh.uv_map_names();
    if (uv_names.is_empty()) {
      return 0.0f;
    }
    StringRefNull uv_name = mmd.sculpt_displacement_uv_layer[0] != '\0' ?
                                StringRefNull(mmd.sculpt_displacement_uv_layer) :
                                mesh.active_uv_map_name();
    if (uv_name.is_empty()) {
      uv_name = uv_names[0];
    }
    const int uv_channel = uv_names.index_of(uv_name);
    if (uv_channel < 0) {
      return 0.0f;
    }
  
    float2 uv;
    bke::subdiv::eval_face_varying(&subdiv, uv_channel, ptex_face_index, u, v, uv);
    tex_co = float3(uv.x, uv.y, 0.0f);
  }
  else {
    /* Sample the undisplaced limit surface so live and bake share a stable
     * baseline, independent of the currently composed sculpt layers. */
    tex_co = bke::subdiv::eval_limit_point(&subdiv, ptex_face_index, u, v);
    const float4x4 *matrix = mapping_matrix ? mapping_matrix :
        bke::subdiv::displacement_multires_texture_matrix(subdiv);
    if (matrix) {
      tex_co = math::transform_point(*matrix, tex_co);
    }
  }
  TexResult texres{};
  BKE_texture_get_value(mmd.sculpt_displacement_texture, tex_co, &texres, false);
  const float delta = (texres.tin - mmd.sculpt_displacement_midlevel) *
                      mmd.sculpt_displacement_strength;
  return std::clamp(delta, -10000.0f, 10000.0f);
}


/* MULTIRES MODIFIER */
static const int multires_grid_tot[] = {
    0, 4, 9, 25, 81, 289, 1089, 4225, 16641, 66049, 263169, 1050625, 4198401, 16785409};
static const int multires_side_tot[] = {
    0, 2, 3, 5, 9, 17, 33, 65, 129, 257, 513, 1025, 2049, 4097};

/** Custom-data. */

void multires_customdata_delete(Mesh *mesh)
{
  if (BMEditMesh *em = mesh->runtime->edit_mesh.get()) {
    /* CustomData_external_remove is used here only to mark layer
     * as non-external for further freeing, so zero element count
     * looks safer than `em->bm->totface`. */
    CustomData_external_remove(&em->bm->ldata, &mesh->id, CD_MDISPS, 0);

    if (CustomData_has_layer(&em->bm->ldata, CD_MDISPS)) {
      BM_data_layer_free(em->bm, &em->bm->ldata, CD_MDISPS);
    }

    if (CustomData_has_layer(&em->bm->ldata, CD_GRID_PAINT_MASK)) {
      BM_data_layer_free(em->bm, &em->bm->ldata, CD_GRID_PAINT_MASK);
    }
  }
  else {
    CustomData_external_remove(&mesh->corner_data, &mesh->id, CD_MDISPS, mesh->corners_num);
    CustomData_free_layer_active(&mesh->corner_data, CD_MDISPS);

    CustomData_free_layer_active(&mesh->corner_data, CD_GRID_PAINT_MASK);
  }
}

static BLI_bitmap *multires_mdisps_downsample_hidden(const BLI_bitmap *old_hidden,
                                                     const int old_level,
                                                     const int new_level)
{
  const int new_gridsize = CCG_grid_size(new_level);
  const int old_gridsize = CCG_grid_size(old_level);

  BLI_assert(new_level <= old_level);
  const int factor = CCG_grid_factor(new_level, old_level);
  BLI_bitmap *new_hidden = BLI_BITMAP_NEW(square_i(new_gridsize), "downsample hidden");

  for (int y = 0; y < new_gridsize; y++) {
    for (int x = 0; x < new_gridsize; x++) {
      const int old_value = BLI_BITMAP_TEST(old_hidden, factor * y * old_gridsize + x * factor);

      BLI_BITMAP_SET(new_hidden, y * new_gridsize + x, old_value);
    }
  }

  return new_hidden;
}

Mesh *BKE_multires_create_mesh(Depsgraph *depsgraph, Object *object, MultiresModifierData *mmd)
{
  Object *object_eval = DEG_get_evaluated(depsgraph, object);
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Mesh *deformed_mesh = bke::mesh_get_eval_deform(
      depsgraph, scene_eval, object_eval, &CD_MASK_BAREMESH);
  ModifierEvalContext modifier_ctx{};
  modifier_ctx.depsgraph = depsgraph;
  modifier_ctx.object = object_eval;
  modifier_ctx.flag = MOD_APPLY_USECACHE | MOD_APPLY_IGNORE_SIMPLIFY;

  const ModifierTypeInfo *mti = BKE_modifier_get_info(mmd->modifier.type);
  Mesh *result = mti->modify_mesh(&mmd->modifier, &modifier_ctx, deformed_mesh);

  if (result == deformed_mesh) {
    result = BKE_mesh_copy_for_eval(*deformed_mesh);
  }
  return result;
}

Array<float3> BKE_multires_create_deformed_base_mesh_vert_coords(Depsgraph *depsgraph,
                                                                 Object *object,
                                                                 MultiresModifierData *mmd)
{
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *object_eval = DEG_get_evaluated(depsgraph, object);

  Object object_for_eval = dna::shallow_copy(*object_eval);
  bke::ObjectRuntime runtime = *object_eval->runtime;
  object_for_eval.runtime = &runtime;

  object_for_eval.data = object->data;
  object_for_eval.runtime->sculpt_session = nullptr;

  const bool use_render = (DEG_get_mode(depsgraph) == DAG_EVAL_RENDER);
  ModifierEvalContext mesh_eval_context = {depsgraph, &object_for_eval, ModifierApplyFlag(0)};
  if (use_render) {
    mesh_eval_context.flag |= MOD_APPLY_RENDER;
  }
  const int required_mode = use_render ? eModifierMode_Render : eModifierMode_Realtime;

  VirtualModifierData virtual_modifier_data;
  ModifierData *first_md = BKE_modifiers_get_virtual_modifierlist(&object_for_eval,
                                                                  &virtual_modifier_data);

  Mesh *base_mesh = id_cast<Mesh *>(object->data);

  Array<float3> deformed_verts(base_mesh->vert_positions());

  for (ModifierData *md = first_md; md != nullptr; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info(md->type);
    if (md == &mmd->modifier) {
      break;
    }
    if (!BKE_modifier_is_enabled(scene_eval, md, required_mode)) {
      continue;
    }
    if (mti->type != ModifierTypeType::OnlyDeform) {
      break;
    }
    BKE_modifier_deform_verts(md, &mesh_eval_context, base_mesh, deformed_verts);
  }

  return deformed_verts;
}

MultiresModifierData *find_multires_modifier_before(Scene *scene, ModifierData *lastmd)
{

  for (ModifierData *md = lastmd; md; md = md->prev) {
    if (md->type == eModifierType_Multires) {
      if (BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
        return reinterpret_cast<MultiresModifierData *>(md);
      }
    }
  }

  return nullptr;
}

MultiresModifierData *get_multires_modifier(Scene *scene, Object *ob, const bool use_first)
{
  MultiresModifierData *mmd = nullptr, *firstmmd = nullptr;

  /* find first active multires modifier */
  for (ModifierData &md : ob->modifiers) {
    if (md.type == eModifierType_Multires) {
      if (!firstmmd) {
        firstmmd = reinterpret_cast<MultiresModifierData *>(&md);
      }

      if (BKE_modifier_is_enabled(scene, &md, eModifierMode_Realtime)) {
        mmd = reinterpret_cast<MultiresModifierData *>(&md);
        break;
      }
    }
  }

  if (!mmd && use_first) {
    /* active multires have not been found
     * try to use first one */
    return firstmmd;
  }

  return mmd;
}

int multires_get_level(const Scene *scene,
                       const Object *ob,
                       const MultiresModifierData *mmd,
                       const bool render,
                       const bool ignore_simplify)
{
  if (render) {
    return (scene != nullptr) ? get_render_subsurf_level(&scene->r, mmd->renderlvl, true) :
                                mmd->renderlvl;
  }
  if (ob->mode == OB_MODE_SCULPT) {
    return mmd->sculptlvl;
  }
  if (ignore_simplify) {
    return mmd->lvl;
  }

  return (scene != nullptr) ? get_render_subsurf_level(&scene->r, mmd->lvl, false) : mmd->lvl;
}

void multires_set_tot_level(Object *ob, MultiresModifierData *mmd, const int lvl)
{
  mmd->totlvl = lvl;

  if (ob->mode != OB_MODE_SCULPT) {
    mmd->lvl = std::clamp<char>(std::max<char>(mmd->lvl, lvl), 0, mmd->totlvl);
  }

  mmd->sculptlvl = std::clamp<char>(std::max<char>(mmd->sculptlvl, lvl), 0, mmd->totlvl);
  mmd->renderlvl = std::clamp<char>(std::max<char>(mmd->renderlvl, lvl), 0, mmd->totlvl);
}

static void multires_ccg_mark_as_modified(SubdivCCG *subdiv_ccg, const MultiresModifiedFlags flags)
{
  if (flags & MULTIRES_COORDS_MODIFIED) {
    subdiv_ccg->dirty.coords = true;
  }
  if (flags & MULTIRES_HIDDEN_MODIFIED) {
    subdiv_ccg->dirty.hidden = true;
  }
  if (flags & MULTIRES_MASK_MODIFIED) {
    subdiv_ccg->dirty.masks = true;
  }
}

void multires_mark_as_modified(Depsgraph *depsgraph,
                               Object *object,
                               const MultiresModifiedFlags flags)
{
  if (object == nullptr) {
    return;
  }
  /* NOTE: CCG live inside of evaluated object.
   *
   * While this is a bit weird to tag the only one, this is how other areas were built
   * historically: they are tagging multires for update and then rely on object re-evaluation to
   * do an actual update.
   *
   * In a longer term maybe special dependency graph tag can help sanitizing this a bit. */
  Object *object_eval = DEG_get_evaluated(depsgraph, object);
  Mesh *mesh = id_cast<Mesh *>(object_eval->data);
  SubdivCCG *subdiv_ccg = mesh->runtime->subdiv_ccg.get();
  if (subdiv_ccg == nullptr) {
    return;
  }
  multires_ccg_mark_as_modified(subdiv_ccg, flags);
}

static bool multires_flush_mask_updates_impl(const SubdivCCG &ccg,
                                               Mesh &mesh,
                                               const int level,
                                               const BitVector<> *dirty_grids)
{
  if (ccg.masks.is_empty()) {
    return true;
  }
  const int side = bke::subdiv::grid_size_from_level(level);
  if (side < 2 || ccg.grid_size < 2 || ccg.grids_num != mesh.corners_num ||
      ccg.masks.size() != ccg.grids_num * ccg.grid_area ||
      (dirty_grids && dirty_grids->size() != ccg.grids_num))
  {
    return false;
  }
  GridPaintMask *dst = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
      &mesh.corner_data, CD_GRID_PAINT_MASK, mesh.corners_num));
  if (!dst) {
    return false;
  }

  const int area = side * side;
  for (int grid = 0; grid < ccg.grids_num; grid++) {
    if (dirty_grids && !(*dirty_grids)[grid]) {
      continue;
    }

    const float *src = ccg.masks.data() + grid * ccg.grid_area;
    /* Validate only the transaction region. A local mask edit should never scan unrelated grids. */
    for (int i = 0; i < ccg.grid_area; i++) {
      if (!std::isfinite(src[i])) {
        return false;
      }
    }

    GridPaintMask &mask = dst[grid];
    if (!mask.data || mask.level != level) {
      MEM_SAFE_DELETE(mask.data);
      mask.data = MEM_new_array_uninitialized<float>(area, "multires mask-only flush");
      mask.level = level;
    }
    for (int y = 0; y < side; y++) {
      const float v = float(y) * float(ccg.grid_size - 1) / float(side - 1);
      const int y0 = int(v), y1 = std::min(y0 + 1, ccg.grid_size - 1);
      const float fy = v - float(y0);
      for (int x = 0; x < side; x++) {
        const float u = float(x) * float(ccg.grid_size - 1) / float(side - 1);
        const int x0 = int(u), x1 = std::min(x0 + 1, ccg.grid_size - 1);
        const float fx = u - float(x0);
        const float a = src[y0 * ccg.grid_size + x0] * (1.0f - fx) +
                        src[y0 * ccg.grid_size + x1] * fx;
        const float b = src[y1 * ccg.grid_size + x0] * (1.0f - fx) +
                        src[y1 * ccg.grid_size + x1] * fx;
        mask.data[y * side + x] = a * (1.0f - fy) + b * fy;
      }
    }
  }
  return true;
}

bool multires_flush_mask_updates(const SubdivCCG &ccg, Mesh &mesh, const int level)
{
  return multires_flush_mask_updates_impl(ccg, mesh, level, nullptr);
}

static void sculpt_change_bits_reset(BitVector<> &bits, const int size)
{
  bits.resize(size, false);
  bits.fill(false);
}

/* V4.4b sparse position journal.
 * Walk only PBVH nodes that actually changed and keep a compact list of touched grids.
 * Sorting/deduplication keeps the path correct even if a grid is ever referenced by more
 * than one changed PBVH node. */
static void sculpt_change_mark_grid_indices(const IndexMask &nodes,
                                            const Span<bke::pbvh::GridsNode> pbvh_nodes,
                                            const int grids_num,
                                            Vector<int> &grids)
{
  grids.clear();
  nodes.foreach_index([&](const int node_i) {
    if (node_i < 0 || node_i >= pbvh_nodes.size()) {
      return;
    }
    for (const int grid : pbvh_nodes[node_i].grids()) {
      if (grid >= 0 && grid < grids_num) {
        grids.append(grid);
      }
    }
  });

  if (grids.size() > 1) {
    std::sort(grids.begin(), grids.end());
    const auto new_end = std::unique(grids.begin(), grids.end());
    grids.resize(int64_t(new_end - grids.begin()));
  }
}
static void sculpt_change_mark_grids(const IndexMask &nodes,
                                     const Span<bke::pbvh::GridsNode> pbvh_nodes,
                                     BitVector<> &grids)
{
  nodes.foreach_index([&](const int node_i) {
    if (node_i < 0 || node_i >= pbvh_nodes.size()) {
      return;
    }
    for (const int grid : pbvh_nodes[node_i].grids()) {
      if (grid >= 0 && grid < grids.size()) {
        grids[grid].set();
      }
    }
  });
}

static int sculpt_change_count(const Vector<int> &grids)
{
  return int(grids.size());
}
static int sculpt_change_count(const BitVector<> &bits)
{
  int count = 0;
  for (const bool value : bits) {
    count += int(value);
  }
  return count;
}

static void multires_sculpt_changes_sync(SculptSession &ss,
                                         bke::pbvh::Tree &pbvh,
                                         const SubdivCCG &ccg)
{
  SculptChangeSet &changes = ss.change_set;
  changes.position_grids.clear();
  sculpt_change_bits_reset(changes.mask_grids, ccg.grids_num);
  sculpt_change_bits_reset(changes.visibility_grids, ccg.grids_num);
  sculpt_change_bits_reset(changes.face_set_grids, ccg.grids_num);
  changes.positions_full = false;
  changes.masks_full = false;
  changes.visibility_full = false;
  changes.face_sets_full = false;
  changes.topology_changed = false;

  IndexMaskMemory position_memory;
  IndexMaskMemory mask_memory;
  IndexMaskMemory visibility_memory;
  IndexMaskMemory face_set_memory;
  IndexMaskMemory topology_memory;
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

  const IndexMask position_nodes = pbvh.changed_nodes(bke::pbvh::ChangeType::Positions,
                                                       position_memory);
  const IndexMask mask_nodes = pbvh.changed_nodes(bke::pbvh::ChangeType::Masks, mask_memory);
  const IndexMask visibility_nodes = pbvh.changed_nodes(bke::pbvh::ChangeType::Visibility,
                                                         visibility_memory);
  const IndexMask face_set_nodes = pbvh.changed_nodes(bke::pbvh::ChangeType::FaceSets,
                                                       face_set_memory);
  const IndexMask topology_nodes = pbvh.changed_nodes(bke::pbvh::ChangeType::Topology,
                                                       topology_memory);

  /* SCULPT_EXACT_DIRTY_GRIDS_CONSUMER_V1_2
   *
   * PBVH position tags are conservative: brush code commonly tags the full
   * node_mask although some grids inside those nodes received zero translation.
   *
   * Validate the exact writer journal as a SUBSET of the PBVH candidate region.
   * If valid, use only exact grids. If the writer did not participate, retain
   * the old PBVH node->grid expansion. */
  const char *exact_env = BLI_getenv("BLENDER_SCULPT_EXACT_GRID_JOURNAL");
  const bool exact_enabled =
      exact_env == nullptr || exact_env[0] == '\0' || exact_env[0] != '0';

  bool used_exact_position_grids = false;
  int exact_candidate_grids = 0;
  int pbvh_candidate_grids = 0;

  if (exact_enabled && !position_nodes.is_empty()) {
    BitVector<> pbvh_position_grids(ccg.grids_num, false);
    sculpt_change_mark_grids(position_nodes, nodes, pbvh_position_grids);
    pbvh_candidate_grids = sculpt_change_count(pbvh_position_grids);

    std::lock_guard<std::mutex> lock(ccg.sculpt_dirty_position_grids_mutex);
    exact_candidate_grids = int(ccg.sculpt_dirty_position_grids.size());

    bool exact_is_valid =
        ccg.sculpt_dirty_position_grid_flags.size() == ccg.grids_num &&
        !ccg.sculpt_dirty_position_grids.is_empty();

    if (exact_is_valid) {
      for (const int grid : ccg.sculpt_dirty_position_grids) {
        if (grid < 0 || grid >= ccg.grids_num || !pbvh_position_grids[grid]) {
          exact_is_valid = false;
          break;
        }
      }
    }

    if (exact_is_valid) {
      changes.position_grids.extend(ccg.sculpt_dirty_position_grids.as_span());
      used_exact_position_grids = true;
    }
    else {
      changes.position_grids.reserve(pbvh_candidate_grids);
      for (const int grid : pbvh_position_grids.index_range()) {
        if (pbvh_position_grids[grid]) {
          changes.position_grids.append(grid);
        }
      }
    }
  }
  else if (!position_nodes.is_empty()) {
    sculpt_change_mark_grid_indices(position_nodes, nodes, ccg.grids_num, changes.position_grids);
    pbvh_candidate_grids = int(changes.position_grids.size());
  }

  if (const char *trace = BLI_getenv("BLENDER_SCULPT_PERF_LOG");
      trace != nullptr && trace[0] != '\0' && trace[0] != '0')
  {
    std::fprintf(stderr,
                 "[SCULPT_EXACT_GRIDS] mode=%s exact=%d pbvh=%d committed=%d nodes=%lld total=%d\n",
                 used_exact_position_grids ? "EXACT" : "PBVH_FALLBACK",
                 exact_candidate_grids,
                 pbvh_candidate_grids,
                 int(changes.position_grids.size()),
                 static_cast<long long>(position_nodes.size()),
                 ccg.grids_num);
  }
  sculpt_change_mark_grids(mask_nodes, nodes, changes.mask_grids);
  sculpt_change_mark_grids(visibility_nodes, nodes, changes.visibility_grids);
  sculpt_change_mark_grids(face_set_nodes, nodes, changes.face_set_grids);

  /* A dirty CCG domain without a PBVH subset is legal (scripts, rebuilds, non-brush tools).
   * Preserve correctness by explicitly promoting only that domain to a full update. */
  changes.positions_full = ccg.dirty.coords && position_nodes.is_empty();
  changes.masks_full = ccg.dirty.masks && mask_nodes.is_empty();
  changes.visibility_full = ccg.dirty.hidden && visibility_nodes.is_empty();
  changes.topology_changed = !topology_nodes.is_empty();
  changes.revision++;
}

static void multires_sculpt_changes_clear(SculptSession &ss, bke::pbvh::Tree &pbvh)
{
  SculptChangeSet &changes = ss.change_set;
  changes.position_grids.clear();
  changes.mask_grids.fill(false);
  changes.visibility_grids.fill(false);
  changes.face_set_grids.fill(false);
  changes.positions_full = false;
  changes.masks_full = false;
  changes.visibility_full = false;
  changes.face_sets_full = false;
  changes.topology_changed = false;

  /* Clear only the recorded exact footprint after a successful flush. */
  if (SubdivCCG *ccg = ss.subdiv_ccg) {
    std::lock_guard<std::mutex> lock(ccg->sculpt_dirty_position_grids_mutex);
    /* SCULPT_EXACT_DIRTY_SAMPLES_CLEAR_V1_1
     * Clear only samples belonging to exact dirty grids. This keeps successful
     * tiny commits O(dirty grids * grid area), not O(total Multires samples). */
    if (ccg->sculpt_dirty_position_sample_flags.size() == ccg->positions.size() &&
        ccg->grid_area > 0)
    {
      for (const int grid : ccg->sculpt_dirty_position_grids) {
        if (grid < 0 || grid >= ccg->grids_num) {
          continue;
        }
        const int64_t sample_base = int64_t(grid) * int64_t(ccg->grid_area);
        for (int i = 0; i < ccg->grid_area; i++) {
          ccg->sculpt_dirty_position_sample_flags[sample_base + i].reset();
        }
      }
    }
    else {
      ccg->sculpt_dirty_position_sample_flags.resize(0);
    }
    if (ccg->sculpt_dirty_position_grid_flags.size() == ccg->grids_num) {
      for (const int grid : ccg->sculpt_dirty_position_grids) {
        if (grid >= 0 && grid < ccg->grids_num) {
          ccg->sculpt_dirty_position_grid_flags[grid] = false;
        }
      }
    }
    else {
      ccg->sculpt_dirty_position_grid_flags = {};
    }
    ccg->sculpt_dirty_position_grids.clear();
  }
  pbvh.clear_changes();
}


enum class MultiresLocalCommitResult {
  NotApplicable,
  Success,
  Failure,
};

/* SCULPT_LOCAL_COMMIT_DEFAULT_ON_V1
 *
 * The local top-level commit path is now the normal path. It already has
 * conservative fallbacks for unsupported cases, uses the compact ChangeSet
 * dirty-grid list and preserves sparse Sculpt Layer payloads.
 *
 * Set BLENDER_SCULPT_LOCAL_COMMIT=0 to force the legacy/global path for A/B
 * testing or emergency fallback. Any other value, or an unset variable,
 * keeps the local path enabled. */
static bool multires_sculpt_local_commit_enabled()
{
  static const bool enabled = []() {
    const char *value = BLI_getenv("BLENDER_SCULPT_LOCAL_COMMIT");
    if (value == nullptr || value[0] == '\0') {
      return true;
    }
    return value[0] != '0';
  }();
  return enabled;
}

/* Conservative V4 fast path: only top-level coordinate-only edits with a known
 * local region. Everything else keeps using the established global V3.1 path. */
/* LAYER_COMMIT_LOCAL_PROFILE_V2C
 * Diagnostic timing only. Does not change local-commit behavior. */
static bool multires_layer_commit_trace_enabled()
{
  const char *value = BLI_getenv("BLENDER_LAYER_RESP_TRACE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/* LAYER_COMMIT_FAST_TANGENT_SOLVE_V1
 *
 * The Multires tangent frame has normalized X/Y axes and a normalized Z axis
 * built from cross(X,Y). Z is therefore orthogonal to the tangent plane while
 * X and Y are not required to be mutually orthogonal.
 *
 * Solve D = X*x + Y*y + Z*z as a 2x2 Gram solve for x/y plus dot(D,Z),
 * avoiding a generic 3x3 matrix inverse for the normal well-conditioned case.
 * Near-degenerate tangent frames keep the established math::invert fallback.
 *
 * Opt-in for the first runtime A/B pass:
 *   BLENDER_SCULPT_FAST_TANGENT_SOLVE=1
 */
static bool multires_sculpt_fast_tangent_solve_enabled()
{
  const char *value = BLI_getenv("BLENDER_SCULPT_FAST_TANGENT_SOLVE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

static float3 multires_sculpt_tangent_solve_fast(const float3x3 &tangent,
                                                 const float3 &object_delta)
{
  const float3 tx = tangent.x_axis();
  const float3 ty = tangent.y_axis();
  const float3 tz = tangent.z_axis();

  const float xy = math::dot(tx, ty);
  const float denom = 1.0f - xy * xy;

  /* Keep the legacy solve for strongly skewed / nearly degenerate frames.
   * The threshold is deliberately conservative for the first A/B build. */
  if (!std::isfinite(denom) || denom <= 1.0e-3f) {
    return math::invert(tangent) * object_delta;
  }

  const float dx = math::dot(object_delta, tx);
  const float dy = math::dot(object_delta, ty);
  const float inv_denom = 1.0f / denom;

  return float3((dx - xy * dy) * inv_denom,
                (dy - xy * dx) * inv_denom,
                math::dot(object_delta, tz));
}

/* LAYER_COMMIT_BASE_FRAME_CACHE_V1
 * LAYER_COMMIT_BASE_FRAME_CACHE_V1_1
 * LAYER_COMMIT_BASE_FRAME_CACHE_V2_RING
 * LAYER_COMMIT_BASE_FRAME_CACHE_V2_1_LINEAR
 * LAYER_COMMIT_BASE_FRAME_CACHE_V3_PRESOLVED
 *
 * V3: cached frames store the already-solved tangent dual basis instead of the
 * original tangent matrix. Cache hits therefore avoid Gram construction/division
 * and execute only three dot products for object->tangent conversion.
 *
 * V2.1: replacement candidates are collected once per Commit. This makes cache
 * lookup O(dirty_grids + cache_capacity), avoiding repeated full-cache scans when
 * a Commit contains more dirty grids than the bounded cache can hold.
 *
 * V2: bounded moving working-set cache. Once full, stale slots not referenced by
 * the current Commit are replaced in ring order. Slots already used by the current
 * Commit are pinned until the solve finishes, so parallel grid solving never aliases
 * two dirty grids to the same cache storage.
 *
 * V1.1: do not key cache validity on MultiresReshapeContext::subdiv pointer.
 * The evaluation context may recreate an equivalent Subdiv evaluator for every Commit,
 * making that pointer transient and forcing a false cache reset. Exact evaluated-cage
 * fingerprint + object/modifier identity + level/layout remain the validity key.
 *
 * Bounded cache for the expensive base-limit evaluation used by local Sculpt Layer Commit.
 * A frame is immutable while the evaluated base cage/topology and Multires level are unchanged.
 * The cache is deliberately single-object/single-layout and bounded by grid count so it cannot
 * silently grow to the size of an entire very-high-resolution Multires surface.
 *
 * First A/B build is opt-in:
 *   BLENDER_SCULPT_COMMIT_FRAME_CACHE=1
 * Optional capacity (grids, default 8192):
 *   BLENDER_SCULPT_COMMIT_FRAME_CACHE_GRIDS=8192
 */
struct MultiresCommitBaseFrame {
  float3 P;
  float3 dual_x;
  float3 dual_y;
  float3 dual_z;
};

/* Pre-solved tangent dual basis for cached Commit frames.
 *
 * For the normal FAST_GRAM case this is algebraically identical to
 * multires_sculpt_tangent_solve_fast(), but moves the tangent-only work out of
 * every later Commit hit. Near-degenerate frames preserve the established
 * generic-inverse fallback exactly. */
static bool multires_sculpt_tangent_dual_fast(const float3x3 &tangent,
                                              float3 &r_dual_x,
                                              float3 &r_dual_y,
                                              float3 &r_dual_z)
{
  const float3 tx = tangent.x_axis();
  const float3 ty = tangent.y_axis();
  const float3 tz = tangent.z_axis();

  const float xy = math::dot(tx, ty);
  const float denom = 1.0f - xy * xy;

  if (std::isfinite(denom) && denom > 1.0e-3f) {
    const float inv_denom = 1.0f / denom;
    r_dual_x = (tx - ty * xy) * inv_denom;
    r_dual_y = (ty - tx * xy) * inv_denom;
    r_dual_z = tz;
    return true;
  }

  const float determinant = math::determinant(tangent);
  if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12f) {
    return false;
  }

  /* Build rows of the inverse without relying on matrix storage order. */
  const float3x3 inv = math::invert(tangent);
  const float3 c0 = inv * float3(1.0f, 0.0f, 0.0f);
  const float3 c1 = inv * float3(0.0f, 1.0f, 0.0f);
  const float3 c2 = inv * float3(0.0f, 0.0f, 1.0f);
  r_dual_x = float3(c0.x, c1.x, c2.x);
  r_dual_y = float3(c0.y, c1.y, c2.y);
  r_dual_z = float3(c0.z, c1.z, c2.z);
  return true;
}

static float3 multires_sculpt_tangent_solve_dual(const MultiresCommitBaseFrame &frame,
                                                 const float3 &object_delta)
{
  return float3(math::dot(object_delta, frame.dual_x),
                math::dot(object_delta, frame.dual_y),
                math::dot(object_delta, frame.dual_z));
}

/* LAYER_COMMIT_FRAME_CACHE_V4_GRID_BATCH_BUILD
 *
 * Build one complete cached base-frame grid while hoisting all metadata which
 * is invariant across its samples. V4.3a made a single sample evaluation
 * cheaper; V4 resolves face/corner/PTex metadata only once per grid instead of
 * once per 17x17 sample.
 *
 * The actual OpenSubdiv limit evaluation and tangent construction are the same
 * operations as multires_reshape_evaluate_base_mesh_limit_at_grid(), so the
 * resulting cached P + dual basis has the same semantics as V3. */
static bool multires_sculpt_commit_build_base_frame_grid_fast(
    const MultiresReshapeContext &context,
    const int grid,
    const int side,
    const float inv_side,
    MultiresCommitBaseFrame *frames)
{
  if (frames == nullptr || side < 2 || grid < 0 || grid >= context.num_grids ||
      context.grid_to_face_index.size() != context.num_grids)
  {
    return false;
  }

  const int face_index = context.grid_to_face_index[grid];
  if (face_index < 0 || face_index >= context.base_faces.size()) {
    return false;
  }

  const int face_start_grid = context.face_start_grid_index[face_index];
  const int corner = grid - face_start_grid;
  const int face_corners = int(context.base_faces[face_index].size());
  if (corner < 0 || corner >= face_corners) {
    return false;
  }

  const bool is_quad = face_corners == 4;
  const int ptex_face_index =
      context.face_ptex_offset[face_index] + (is_quad ? 0 : corner);
  const int tangent_corner = is_quad ? corner : 0;

  for (int y = 0; y < side; y++) {
    const float grid_v = float(y) * inv_side;
    for (int x = 0; x < side; x++) {
      const int i = y * side + x;
      const float grid_u = float(x) * inv_side;

      float ptex_u;
      float ptex_v;
      if (is_quad) {
        bke::subdiv::rotate_grid_to_quad(
            corner, grid_u, grid_v, &ptex_u, &ptex_v);
      }
      else {
        bke::subdiv::grid_uv_to_ptex_face_uv(
            grid_u, grid_v, &ptex_u, &ptex_v);
      }

      MultiresCommitBaseFrame &frame = frames[i];
      float3 dPdu;
      float3 dPdv;
      bke::subdiv::eval_limit_point_and_derivatives(context.subdiv,
                                                     ptex_face_index,
                                                     ptex_u,
                                                     ptex_v,
                                                     frame.P,
                                                     dPdu,
                                                     dPdv);

      float3x3 tangent;
      BKE_multires_construct_tangent_matrix(
          tangent, dPdu, dPdv, tangent_corner);

      if (!multires_sculpt_tangent_dual_fast(
              tangent, frame.dual_x, frame.dual_y, frame.dual_z))
      {
        return false;
      }
    }
  }
  return true;
}


struct MultiresCommitBaseFrameCache {
  const Mesh *mesh = nullptr;
  const MultiresModifierData *mmd = nullptr;
  uint64_t fingerprint = 0;
  int level = -1;
  int side = 0;
  int area = 0;
  int grids_num = 0;
  int capacity_grids = 0;
  int cached_grids = 0;
  int next_replace_slot = 0;
  std::unique_ptr<int[]> grid_slots;
  std::unique_ptr<int[]> slot_grids;
  std::unique_ptr<uint8_t[]> slot_ready;
  std::unique_ptr<MultiresCommitBaseFrame[]> frames;

  void configure(const Mesh &new_mesh,
                 const MultiresModifierData &new_mmd,
                 const uint64_t new_fingerprint,
                 const int new_level,
                 const int new_side,
                 const int new_area,
                 const int new_grids_num,
                 const int new_capacity_grids)
  {
    const bool same_layout = area == new_area && grids_num == new_grids_num &&
                             capacity_grids == new_capacity_grids && grid_slots &&
                             (new_capacity_grids == 0 || slot_grids) &&
                             slot_ready && (new_capacity_grids == 0 || frames);

    if (!same_layout) {
      grid_slots.reset(new_capacity_grids > 0 ? new int[new_grids_num] : nullptr);
      slot_grids.reset(new_capacity_grids > 0 ? new int[new_capacity_grids] : nullptr);
      slot_ready.reset(new_capacity_grids > 0 ? new uint8_t[new_capacity_grids] : nullptr);
      frames.reset(new_capacity_grids > 0 ?
                       new MultiresCommitBaseFrame[int64_t(new_capacity_grids) * new_area] :
                       nullptr);
    }

    if (new_capacity_grids > 0) {
      std::fill_n(grid_slots.get(), new_grids_num, -1);
    }
    if (new_capacity_grids > 0) {
      std::fill_n(slot_grids.get(), new_capacity_grids, -1);
      std::fill_n(slot_ready.get(), new_capacity_grids, uint8_t(0));
    }

    mesh = &new_mesh;
    mmd = &new_mmd;
    fingerprint = new_fingerprint;
    level = new_level;
    side = new_side;
    area = new_area;
    grids_num = new_grids_num;
    capacity_grids = new_capacity_grids;
    cached_grids = 0;
    next_replace_slot = 0;
  }

  MultiresCommitBaseFrame *slot_frames(const int slot)
  {
    return frames.get() + int64_t(slot) * int64_t(area);
  }
};

static MultiresCommitBaseFrameCache &multires_sculpt_commit_base_frame_cache()
{
  static MultiresCommitBaseFrameCache cache;
  return cache;
}

static std::mutex &multires_sculpt_commit_base_frame_cache_mutex()
{
  static std::mutex mutex;
  return mutex;
}

static bool multires_sculpt_commit_base_frame_cache_enabled()
{
  const char *value = BLI_getenv("BLENDER_SCULPT_COMMIT_FRAME_CACHE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/* Opt-in total budget. Absent preserves the validated legacy policy.
 * Read once per process; restart Blender after changing the budget. */
static int64_t multires_layer_cache_budget_bytes()
{
  static const int64_t bytes = []() -> int64_t {
    const char *value = BLI_getenv("BLENDER_SCULPT_LAYER_CACHE_MB");
    if (value == nullptr || value[0] == '\0') {
      return -1;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0) {
      return -1;
    }
    return int64_t(std::min<long>(parsed, 32768)) * 1024 * 1024;
  }();
  return bytes;
}

static int multires_sculpt_commit_base_frame_cache_capacity()
{
  constexpr int default_capacity = 8192;
  constexpr int max_capacity = 32768;
  const char *value = BLI_getenv("BLENDER_SCULPT_COMMIT_FRAME_CACHE_GRIDS");
  if (value == nullptr || value[0] == '\0') {
    return default_capacity;
  }
  char *end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) {
    return default_capacity;
  }
  return std::clamp(int(parsed), 0, max_capacity);
}

static void multires_commit_hash_u32(uint64_t &hash, const uint32_t value)
{
  hash ^= uint64_t(value);
  hash *= UINT64_C(1099511628211);
}

static void multires_commit_hash_int(uint64_t &hash, const int value)
{
  multires_commit_hash_u32(hash, uint32_t(value));
}

static void multires_commit_hash_float(uint64_t &hash, const float value)
{
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  multires_commit_hash_u32(hash, bits);
}

static uint64_t multires_sculpt_commit_base_fingerprint(const MultiresReshapeContext &context,
                                                         const int level)
{
  uint64_t hash = UINT64_C(1469598103934665603);
  multires_commit_hash_int(hash, level);
  multires_commit_hash_int(hash, int(context.base_positions.size()));
  multires_commit_hash_int(hash, int(context.base_edges.size()));
  multires_commit_hash_int(hash, int(context.base_faces.size()));
  multires_commit_hash_int(hash, int(context.base_corner_verts.size()));
  multires_commit_hash_int(hash, int(context.base_corner_edges.size()));

  /* Hash the exact evaluated cage used to build the limit surface. This catches Edit Mode,
   * shape/deform changes before Multires and Conform Base without depending on a fragile
   * external revision counter. */
  for (const float3 &P : context.base_positions) {
    multires_commit_hash_float(hash, P.x);
    multires_commit_hash_float(hash, P.y);
    multires_commit_hash_float(hash, P.z);
  }
  for (const int2 &edge : context.base_edges) {
    multires_commit_hash_int(hash, edge.x);
    multires_commit_hash_int(hash, edge.y);
  }
  for (const int v : context.base_corner_verts) {
    multires_commit_hash_int(hash, v);
  }
  for (const int e : context.base_corner_edges) {
    multires_commit_hash_int(hash, e);
  }
  for (const int face : context.base_faces.index_range()) {
    const IndexRange range = context.base_faces[face];
    multires_commit_hash_int(hash, int(range.start()));
    multires_commit_hash_int(hash, int(range.size()));
  }
  return hash;
}


/* LAYER_VISIBLE_DIRECT_DELTA_V2_1_BKE */
/* LAYER_WEIGHT_INCREMENTAL_V2_2
 * One engine for Visible and Strength. The layer payload is tangent-space;
 * weight_delta is simply new_weight-old_weight.
 *
 * Cache hits reuse the Commit V3 pre-solved dual basis. The primal tangent
 * basis is reconstructed from the reciprocal basis with cross products, which
 * is far cheaper than another OpenSubdiv limit/derivative evaluation.
 */

/* LAYER_WEIGHT_HOT_OBJECT_DELTA_CACHE_V1 */
struct MultiresWeightHotDeltaCache {
  const Mesh *mesh=nullptr;
  const MultiresModifierData *mmd=nullptr;
  const MultiresSculptLayer *layer=nullptr;
  const MDisps *layer_disps=nullptr;
  uint64_t fingerprint=0;
  int level=-1, side=0, area=0, grids_num=0;
  Array<int> grids;
  Array<int> grid_offsets;
  Array<float3> deltas;
  void clear()
  {
    mesh=nullptr; mmd=nullptr; layer=nullptr; layer_disps=nullptr;
    fingerprint=0; level=-1; side=area=grids_num=0;
    grids={}; grid_offsets={}; deltas={};
  }
};
static MultiresWeightHotDeltaCache &multires_weight_hot_delta_cache()
{
  static MultiresWeightHotDeltaCache cache;
  return cache;
}
/* LAYER_WEIGHT_MULTI_CACHE_V01: bounded, opt-in; legacy engine remains available. */
static bool multires_weight_multi_cache_enabled()
{
  static const bool enabled = []() {
    const char *v = BLI_getenv("BLENDER_SCULPT_WEIGHT_MULTI_CACHE");
    return v && v[0] == '1' && v[1] == '\0';
  }();
  return enabled;
}

static int64_t multires_weight_multi_cache_budget()
{
  static const int64_t bytes = []() -> int64_t {
    const char *v = BLI_getenv("BLENDER_SCULPT_WEIGHT_MULTI_CACHE_MB");
    if (!v || !v[0]) return int64_t(2048) * 1024 * 1024;
    char *end = nullptr;
    const long mb = std::strtol(v, &end, 10);
    /* Invalid settings disable the new cache rather than request unbounded RAM. */
    if (end == v || *end != '\0' || mb < 0 || mb > 8192) return 0;
    return int64_t(mb) * 1024 * 1024;
  }();
  return bytes;
}

using MultiresWeightMultiCache =
    bke::multires_layers::weight_multi_v01::BoundedLRU<MultiresWeightHotDeltaCache>;
static MultiresWeightMultiCache &multires_weight_multi_cache()
{
  static MultiresWeightMultiCache cache;
  return cache;
}

static std::mutex &multires_weight_hot_delta_cache_mutex()
{
  static std::mutex mutex;
  return mutex;
}

void bke::multires_layers::weight_hot_delta_cache_invalidate(
    const MultiresSculptLayer *layer)
{
  blender::bke::sculpt_cold_v01::Scope cold_trace("layer.invalidate_weight_cache", layer);
  payload_index_invalidate(layer);
  std::unique_lock<std::mutex> lock(multires_weight_hot_delta_cache_mutex());
  MultiresWeightHotDeltaCache &hot = multires_weight_hot_delta_cache();
  const int removed = multires_weight_multi_cache().invalidate(
      [&](const MultiresWeightHotDeltaCache &entry) {
        return layer == nullptr || entry.layer == layer;
      });
  const bool clear_legacy = layer == nullptr || hot.layer == layer;
  const bool had_cache = removed > 0 || (clear_legacy && hot.layer != nullptr);
  if (clear_legacy) hot.clear();
  if (blender::bke::sculpt_cold_v01::path()) {
    blender::bke::sculpt_cold_v01::event("WEIGHT_CACHE_INVALIDATE", "scope=%s layer=%p removed=%d legacy=%d entries_left=%d",
      layer ? "LAYER" : "ALL", static_cast<const void *>(layer), removed, int(clear_legacy),
      multires_weight_multi_cache().size());
  }
  if (had_cache && BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
    std::printf("[LAYER_WEIGHT_HOT_DELTA] mode=INVALIDATE scope=%s\n",
                layer != nullptr ? "LAYER" : "ALL");
  }
}

static bool multires_weight_hot_delta_cache_enabled()
{
  const char *v=BLI_getenv("BLENDER_SCULPT_WEIGHT_HOT_DELTA_CACHE");
  return v==nullptr || v[0]=='\0' || v[0]!='0';
}

bool bke::multires_layers::weight_direct_delta(Object &object,
                                               MultiresModifierData &mmd,
                                               const MultiresSculptLayer &layer,
                                               const float weight_delta,
                                               BitVector<> &r_affected_faces,
                                               bool *r_hot_cache_hit)
{
  if (r_hot_cache_hit != nullptr) {
    *r_hot_cache_hit = false;
  }
  const char *env=BLI_getenv("BLENDER_SCULPT_VISIBLE_DIRECT_DELTA");
  if (env!=nullptr && env[0]!='\0' && env[0]=='0') return false;
  if (!std::isfinite(weight_delta) || !std::isfinite(layer.strength) ||
      object.runtime==nullptr || object.runtime->sculpt_session==nullptr ||
      mmd.sculptlvl!=mmd.totlvl || layer.level!=mmd.totlvl || layer.disps==nullptr)
    return false;

  SculptSession &ss=*object.runtime->sculpt_session;
  SubdivCCG *ccg=ss.subdiv_ccg;
  Mesh *mesh=BKE_mesh_from_object(&object);
  if (ccg==nullptr || mesh==nullptr || ccg->subdiv==nullptr ||
      ccg->level!=mmd.totlvl || layer.grids_num!=ccg->grids_num ||
      ccg->grids_num!=mesh->corners_num)
    return false;

  const int side=ccg->grid_size, area=ccg->grid_area;
  if (side<2 || area!=side*side) return false;

  r_affected_faces.resize(mesh->faces_num);
  r_affected_faces.fill(false);
  BitVector<> affected_grids(ccg->grids_num,false);
  Vector<int> payload_grids;
  if (!bke::multires_layers::payload_grids(layer, ccg->grids_num, payload_grids)) {
    return false;
  }
  const OffsetIndices faces = mesh->faces();
  for (const int grid : payload_grids) {
    affected_grids[grid].set();
    r_affected_faces[int(std::upper_bound(faces.data().begin(), faces.data().end(), grid) - faces.data().begin() - 1)].set();
  }
  if (payload_grids.is_empty() || std::abs(weight_delta)<1.0e-12f) return true;

  MultiresReshapeContext context{};
  const char *readonly_env = BLI_getenv("BLENDER_SCULPT_LAYER_READONLY_WEIGHT");
  const bool readonly_weight = readonly_env && readonly_env[0] == '1' && readonly_env[1] == '\0';
  const double context_start = BLI_time_now_seconds();
  /* Weight updates evaluate tangents and read topology, never write native
   * displacement/mask storage. Reuse the read-only CCG initializer already
   * used by local Commit instead of requesting writable CustomData layers. */
  const bool context_ok = readonly_weight ?
      (CustomData_get_layer(&mesh->corner_data, CD_MDISPS) != nullptr &&
       multires_reshape_context_create_from_ccg_evaluation(&context, ccg, mesh, mmd.totlvl)) :
      multires_reshape_context_create_from_subdiv(&context, &object, &mmd, ccg->subdiv, mmd.totlvl);
  if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
    std::printf("[LAYER_WEIGHT_CONTEXT] mode=%s ok=%d init=%.3fms\n",
                readonly_weight ? "READ_ONLY" : "LEGACY_WRITABLE", int(context_ok),
                (BLI_time_now_seconds() - context_start) * 1000.0);
    std::fflush(stdout);
  }
  if (!context_ok)
    return false;
  BLI_SCOPED_DEFER([&](){ multires_reshape_context_free(&context); });

  const uint64_t fingerprint=multires_sculpt_commit_base_fingerprint(context,mmd.totlvl);
  const int64_t budget = multires_layer_cache_budget_bytes();
  const bool budget_fits = bke::multires_layers::cache_budget::weight_fits(
      int(payload_grids.size()), area, ccg->grids_num, budget);
  bool hot_enabled = multires_weight_hot_delta_cache_enabled() && budget_fits;
  std::unique_lock<std::mutex> hot_lock(multires_weight_hot_delta_cache_mutex());
  MultiresWeightHotDeltaCache *selected = &multires_weight_hot_delta_cache();
  const bool multi = multires_weight_multi_cache_enabled();
  int multi_evicted = 0;
  int multi_invalidated = 0;
  if (multi) {
    /* Never retain the old singleton in addition to the new budget. */
    selected->clear();
    MultiresWeightMultiCache &pool = multires_weight_multi_cache();
    /* Conservative single-object/base/layout scope. Fingerprints read the exact
     * evaluated cage, not the temporary evaluator pointer. Layer mutations still
     * use all existing explicit invalidation hooks (including Undo/Redo). */
    multi_invalidated = pool.invalidate([&](const MultiresWeightHotDeltaCache &entry) {
      return entry.mesh != mesh || entry.mmd != &mmd || entry.fingerprint != fingerprint ||
             entry.level != mmd.totlvl || entry.side != side || entry.area != area ||
             entry.grids_num != ccg->grids_num ||
             (entry.layer == &layer &&
              (entry.layer_disps != layer.disps || entry.grids.size() != payload_grids.size()));
    });
    hot_enabled = multires_weight_hot_delta_cache_enabled();
    if (hot_enabled) {
      MultiresWeightHotDeltaCache *entry = pool.find(
          [&](const MultiresWeightHotDeltaCache &candidate) { return candidate.layer == &layer; });
      if (!entry) {
        const int64_t charge = int64_t(sizeof(MultiresWeightHotDeltaCache)) + 128 +
            int64_t(payload_grids.size()) * sizeof(int) +
            int64_t(ccg->grids_num) * sizeof(int) +
            int64_t(payload_grids.size()) * int64_t(area) * sizeof(float3);
        entry = pool.insert(charge, multires_weight_multi_cache_budget(), multi_evicted);
      }
      if (entry) selected = entry;
      else hot_enabled = false;
    }
    else {
      pool.invalidate([](const MultiresWeightHotDeltaCache &) { return true; });
    }
  }
  MultiresWeightHotDeltaCache &hot = *selected;

  bool exact_hit=hot_enabled && hot.mesh==mesh && hot.mmd==&mmd &&
      hot.layer==&layer && hot.layer_disps==layer.disps && hot.fingerprint==fingerprint &&
      hot.level==mmd.totlvl && hot.side==side && hot.area==area &&
      hot.grids_num==ccg->grids_num && hot.grids.size()==payload_grids.size() &&
      hot.grid_offsets.size()==ccg->grids_num;
  if (exact_hit) {
    for (const int grid: payload_grids) {
      if (hot.grid_offsets[grid]<0) { exact_hit=false; break; }
    }
  }
  if (r_hot_cache_hit != nullptr) {
    *r_hot_cache_hit = exact_hit;
  }

  if (!hot_enabled && !multi && budget >= 0) {
    hot.clear();
    if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
      std::printf("[LAYER_CACHE_BUDGET] weight=BYPASS budget=%.1fMiB grids=%d\n",
                  double(budget) / (1024.0 * 1024.0), int(payload_grids.size()));
    }
  }
  const double hot_start=BLI_time_now_seconds();
  if (!exact_hit && hot_enabled) {
    hot.clear();
    hot.mesh=mesh; hot.mmd=&mmd; hot.layer=&layer; hot.layer_disps=layer.disps;
    hot.fingerprint=fingerprint; hot.level=mmd.totlvl; hot.side=side;
    hot.area=area; hot.grids_num=ccg->grids_num; hot.grids=Array<int>(payload_grids.as_span());
    hot.grid_offsets=Array<int>(ccg->grids_num); hot.grid_offsets.fill(-1);
    hot.deltas=Array<float3>(int64_t(payload_grids.size())*int64_t(area));
    for (const int j: payload_grids.index_range()) hot.grid_offsets[payload_grids[j]]=j*area;

    const float inv=1.0f/float(side-1);
    threading::parallel_for(payload_grids.index_range(),1,[&](const IndexRange range){
      for (const int j: range) {
        const int grid=payload_grids[j];
        const MDisps &lg=layer.disps[grid];
        MutableSpan<float3> dst=hot.deltas.as_mutable_span().slice(int64_t(j)*area,area);
        for (int y=0;y<side;y++) for (int x=0;x<side;x++) {
          const int i=y*side+x;
          const GridCoord coord{grid,float(x)*inv,float(y)*inv};
          float3 P; float3x3 tangent;
          multires_reshape_evaluate_base_mesh_limit_at_grid(&context,&coord,P,tangent);
          dst[i]=tangent*float3(lg.disps[i]);
        }
      }
    });
  }

  if (hot_enabled) {
    threading::parallel_for(payload_grids.index_range(),1,[&](const IndexRange range){
      for (const int j: range) {
        const int grid=payload_grids[j];
        const int off=hot.grid_offsets[grid];
        MutableSpan<float3> pos=ccg->positions.as_mutable_span().slice(int64_t(grid)*area,area);
        Span<float3> d=hot.deltas.as_span().slice(off,area);
        for (const int i: IndexRange(area)) pos[i]+=d[i]*weight_delta;
      }
    });
    if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
      std::printf("[LAYER_WEIGHT_HOT_DELTA] mode=%s grids=%d samples=%lld bytes=%.1fMiB ms=%.3f\n",
                  exact_hit ? "HIT" : "BUILD", int(payload_grids.size()),
                  (long long)payload_grids.size()*area,
                  double(payload_grids.size())*double(area)*sizeof(float3)/(1024.0*1024.0),
                  (BLI_time_now_seconds()-hot_start)*1000.0);
    }
    if (multi && BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
      const MultiresWeightMultiCache &pool = multires_weight_multi_cache();
      std::printf("[LAYER_WEIGHT_MULTI_V01] mode=%s entries=%d used_mib=%.3f budget_mib=%.1f evicted=%d invalidated=%d\n",
                  exact_hit ? "HIT" : "BUILD", pool.size(),
                  double(pool.bytes()) / (1024.0 * 1024.0),
                  double(multires_weight_multi_cache_budget()) / (1024.0 * 1024.0),
                  multi_evicted, multi_invalidated);
    }
    return true;
  }

  if (multi && BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
    const MultiresWeightMultiCache &pool = multires_weight_multi_cache();
    std::printf("[LAYER_WEIGHT_MULTI_V01] mode=BYPASS entries=%d used_mib=%.3f budget_mib=%.1f\n",
                pool.size(), double(pool.bytes()) / (1024.0 * 1024.0),
                double(multires_weight_multi_cache_budget()) / (1024.0 * 1024.0));
  }
  const float inv=1.0f/float(side-1);
  threading::parallel_for(payload_grids.index_range(),1,[&](const IndexRange range){
    for (const int j: range) {
      const int grid=payload_grids[j];
      const MDisps &lg=layer.disps[grid];
      MutableSpan<float3> pos=ccg->positions.as_mutable_span().slice(int64_t(grid)*area,area);
      for (int y=0;y<side;y++) for (int x=0;x<side;x++) {
        const int i=y*side+x;
        const GridCoord coord{grid,float(x)*inv,float(y)*inv};
        float3 P; float3x3 tangent;
        multires_reshape_evaluate_base_mesh_limit_at_grid(&context,&coord,P,tangent);
        pos[i]+=(tangent*float3(lg.disps[i]))*weight_delta;
      }
    }
  });
  return true;
}


bool bke::multires_layers::visible_direct_delta(Object &object,
                                                MultiresModifierData &mmd,
                                                const MultiresSculptLayer &layer,
                                                const bool make_visible,
                                                BitVector<> &r_affected_faces)
{
  return weight_direct_delta(object,
                             mmd,
                             layer,
                             (make_visible ? 1.0f : -1.0f)*layer.strength,
                             r_affected_faces);
}

static MultiresLocalCommitResult multires_sculpt_layer_commit_top_level_local(
    Mesh &mesh,
    MultiresModifierData &mmd,
    MultiresSculptLayer &layer,
    SubdivCCG &ccg,
    const SculptChangeSet &changes)
{
  const bool commit_trace = multires_layer_commit_trace_enabled();
  const bool use_fast_tangent_solve = multires_sculpt_fast_tangent_solve_enabled();
  const double commit_total_start = commit_trace ? BLI_time_now_seconds() : 0.0;

  if (!multires_sculpt_local_commit_enabled() || changes.positions_full ||
      changes.topology_changed || ccg.dirty.hidden || ccg.dirty.masks ||
      ccg.level != mmd.totlvl || mmd.sculptlvl != mmd.totlvl || mmd.totlvl <= 0)
  {
    return MultiresLocalCommitResult::NotApplicable;
  }

  const int dirty_grids = sculpt_change_count(changes.position_grids);
  if (dirty_grids == 0 || dirty_grids * 2 > ccg.grids_num) {
    return MultiresLocalCommitResult::NotApplicable;
  }

  const int side = bke::subdiv::grid_size_from_level(mmd.totlvl);
  const int area = side * side;
  if (side < 2 || ccg.grid_size != side || ccg.grid_area != area ||
      ccg.grids_num != mesh.corners_num ||
      ccg.positions.size() != int64_t(ccg.grids_num) * int64_t(area) ||
      changes.position_grids.size() > ccg.grids_num || !bke::multires_layers::is_effective(layer))
  {
    return MultiresLocalCommitResult::Failure;
  }

  const MDisps *base = static_cast<const MDisps *>(
      CustomData_get_layer(&mesh.corner_data, CD_MDISPS));
  if (!base || !layer.disps || layer.grids_num != ccg.grids_num || layer.level != mmd.totlvl) {
    return MultiresLocalCommitResult::Failure;
  }

  Vector<const MultiresSculptLayer *> other_layers;
  other_layers.reserve(std::max(0, mmd.sculpt_layers.count() - 1));
  for (const MultiresSculptLayer &candidate : mmd.sculpt_layers) {
    if (&candidate != &layer && bke::multires_layers::is_effective(candidate)) {
      other_layers.append(&candidate);
    }
  }
  /* V4.4b: ChangeSet already contains the compact dirty-grid list.
   * Never scan 0..ccg.grids_num for a local sculpt transaction. */
  const Span<int> grids = changes.position_grids.as_span();
  for (const int grid : grids) {
    if (grid < 0 || grid >= ccg.grids_num) {
      return MultiresLocalCommitResult::Failure;
    }
  }


  /* SCULPT_EXACT_DIRTY_SAMPLES_CONSUMER_V1_1
   *
   * The Exact Dirty Grid journal tells us which grids really moved. The sample
   * journal narrows that footprint again to the positions that received a non-zero
   * translation. If the journals do not match exactly, fall back to solving every
   * sample of every dirty grid. This keeps unsupported/direct writers conservative. */
  const char *sample_env = BLI_getenv("BLENDER_SCULPT_EXACT_SAMPLE_JOURNAL");
  const bool sample_journal_enabled =
      sample_env == nullptr || sample_env[0] == '\0' || sample_env[0] != '0';
  const char *grid_env = BLI_getenv("BLENDER_SCULPT_EXACT_GRID_JOURNAL");
  const bool grid_journal_enabled =
      grid_env == nullptr || grid_env[0] == '\0' || grid_env[0] != '0';

  const int64_t full_sample_count = int64_t(grids.size()) * int64_t(area);
  int64_t solved_sample_count = full_sample_count;
  bool use_exact_samples = false;

  /* LAYER_COMMIT_DENSE_SAMPLE_BYPASS_V1
   *
   * Cached V3 grids with an almost-full exact footprint can solve the whole
   * grid faster than checking one BitVector flag per sample. We keep exact
   * publishing, so untouched samples are never written back by this fast path.
   *
   * 97.5% is intentionally conservative: uncached/general evaluator grids
   * always keep exact skipping, and sparse strokes keep the existing journal. */
  const char *dense_sample_env = BLI_getenv("BLENDER_SCULPT_DENSE_SAMPLE_BYPASS");
  const bool dense_sample_bypass_enabled =
      dense_sample_env == nullptr || dense_sample_env[0] == '\0' || dense_sample_env[0] != '0';
  const int dense_sample_threshold = int((int64_t(area) * 975 + 999) / 1000);
  Array<int> exact_sample_counts(grids.size(), area);

  std::unique_lock<std::mutex> exact_sample_lock;

  if (sample_journal_enabled && grid_journal_enabled) {
    exact_sample_lock = std::unique_lock<std::mutex>(ccg.sculpt_dirty_position_grids_mutex);

    bool journals_match =
        ccg.sculpt_dirty_position_sample_flags.size() == ccg.positions.size() &&
        ccg.sculpt_dirty_position_grid_flags.size() == ccg.grids_num &&
        ccg.sculpt_dirty_position_grids.size() == grids.size();

    if (journals_match) {
      for (const int i : grids.index_range()) {
        if (ccg.sculpt_dirty_position_grids[i] != grids[i] ||
            !ccg.sculpt_dirty_position_grid_flags[grids[i]])
        {
          journals_match = false;
          break;
        }
      }
    }

    int64_t exact_samples = 0;
    if (journals_match) {
      for (const int dirty_i : grids.index_range()) {
        const int grid = grids[dirty_i];
        const int64_t sample_base = int64_t(grid) * int64_t(area);
        int grid_exact_samples = 0;
        for (int i = 0; i < area; i++) {
          if (ccg.sculpt_dirty_position_sample_flags[sample_base + i]) {
            exact_samples++;
            grid_exact_samples++;
          }
        }
        exact_sample_counts[dirty_i] = grid_exact_samples;
        if (grid_exact_samples == 0) {
          journals_match = false;
          break;
        }
      }
    }

    if (journals_match && exact_samples > 0 && exact_samples <= full_sample_count) {
      use_exact_samples = true;
      solved_sample_count = exact_samples;
    }
    else {
      exact_sample_lock.unlock();
    }
  }

  if (commit_trace) {
    const double reduction = full_sample_count > 0 ?
                                 100.0 * (1.0 - double(solved_sample_count) /
                                                   double(full_sample_count)) :
                                 0.0;
    std::printf("[SCULPT_EXACT_SAMPLES] mode=%s solved=%lld/%lld reduction=%.1f%%\n",
                use_exact_samples ? "EXACT" : "GRID_FALLBACK",
                static_cast<long long>(solved_sample_count),
                static_cast<long long>(full_sample_count),
                reduction);
  }

  /* Validate all persistent inputs before staging output. */
  for (const int grid : grids) {
    if (/* V4.5a sparse Sculpt Layer foundation: null layer payload == implicit zero grid. */
        !base[grid].disps || base[grid].level != mmd.totlvl || base[grid].totdisp != area ||
        layer.disps[grid].level != mmd.totlvl || layer.disps[grid].totdisp != area)
    {
      return MultiresLocalCommitResult::Failure;
    }
    for (const MultiresSculptLayer *other : other_layers) {
      if (!other->disps || grid >= other->grids_num ||
          other->disps[grid].level != mmd.totlvl || other->disps[grid].totdisp != area)
      {
        return MultiresLocalCommitResult::Failure;
      }
    }
  }
  const double commit_context_start = commit_trace ? BLI_time_now_seconds() : 0.0;
  MultiresReshapeContext context{};
  if (!multires_reshape_context_create_from_ccg_evaluation(&context, &ccg, &mesh, mmd.totlvl)) {
    return MultiresLocalCommitResult::Failure;
  }
  BLI_SCOPED_DEFER([&]() { multires_reshape_context_free(&context); });

  const bool commit_frame_cache_enabled = multires_sculpt_commit_base_frame_cache_enabled() && use_fast_tangent_solve;

  const char *commit_frame_batch_env = BLI_getenv("BLENDER_SCULPT_COMMIT_BATCHED_FRAME_BUILD");

  const bool commit_frame_batch_build_enabled =

      commit_frame_cache_enabled &&

      (commit_frame_batch_env == nullptr || commit_frame_batch_env[0] == '\0' ||

       commit_frame_batch_env[0] != '0');
  const double commit_frame_cache_start = commit_trace ? BLI_time_now_seconds() : 0.0;
  std::unique_lock<std::mutex> commit_frame_cache_lock;
  Array<MultiresCommitBaseFrame *> commit_frame_grids(grids.size(), nullptr);
  Array<uint8_t *> commit_frame_ready(grids.size(), nullptr);
  Array<int> commit_frame_build(grids.size(), 0);
  int commit_frame_hit_grids = 0;
  int commit_frame_build_grids = 0;
  int commit_frame_bypass_grids = int(grids.size());
  int commit_frame_replace_grids = 0;
  int commit_frame_cached_grids = 0;
  int commit_frame_capacity = 0;
  bool commit_frame_cache_reset = false;

  if (commit_frame_cache_enabled) {
    commit_frame_cache_lock =
        std::unique_lock<std::mutex>(multires_sculpt_commit_base_frame_cache_mutex());
    MultiresCommitBaseFrameCache &frame_cache = multires_sculpt_commit_base_frame_cache();
    const uint64_t fingerprint = multires_sculpt_commit_base_fingerprint(context, mmd.totlvl);
    commit_frame_capacity = bke::multires_layers::cache_budget::frame_capacity(
        multires_sculpt_commit_base_frame_cache_capacity(),
        area,
        ccg.grids_num,
        multires_layer_cache_budget_bytes(),
        sizeof(MultiresCommitBaseFrame));
    if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
      std::printf("[LAYER_CACHE_BUDGET] frame_capacity=%d budget=%.1fMiB\n",
                  commit_frame_capacity,
                  double(multires_layer_cache_budget_bytes()) / (1024.0 * 1024.0));
    }

    const bool cache_matches = frame_cache.mesh == &mesh && frame_cache.mmd == &mmd &&
                               frame_cache.fingerprint == fingerprint &&
                               frame_cache.level == mmd.totlvl && frame_cache.side == side &&
                               frame_cache.area == area && frame_cache.grids_num == ccg.grids_num &&
                               frame_cache.capacity_grids == commit_frame_capacity;
    if (!cache_matches) {
      frame_cache.configure(mesh,
                            mmd,
                            fingerprint,
                            mmd.totlvl,
                            side,
                            area,
                            ccg.grids_num,
                            commit_frame_capacity);
      commit_frame_cache_reset = true;
    }

    /* V2.1 bounded moving working set.
     *
     * Pass 1 resolves every cache hit first and pins those slots for the duration
     * of this Commit. This guarantees a later miss can never evict storage that
     * another dirty grid in the same parallel solve will read.
     *
     * Pass 2 builds the replacement pool once: unused slots first, then stale
     * unpinned slots in ring order. Each cache slot is inspected at most once.
     * Remaining misses simply bypass when the Commit footprint is larger than
     * the configured cache capacity. */
    Array<uint8_t> commit_frame_slot_pinned(commit_frame_capacity);
    commit_frame_slot_pinned.fill(uint8_t(0));

    Vector<int> commit_frame_misses;
    commit_frame_misses.reserve(grids.size());

    commit_frame_bypass_grids = 0;

    /* Pass 1: resolve and pin all hits before any eviction is allowed. */
    for (const int dirty_i : grids.index_range()) {
      const int grid = grids[dirty_i];
      const int slot = frame_cache.capacity_grids > 0 ? frame_cache.grid_slots[grid] : -1;

      if (slot < 0) {
        commit_frame_misses.append(dirty_i);
        continue;
      }

      commit_frame_slot_pinned[slot] = 1;
      commit_frame_grids[dirty_i] = frame_cache.slot_frames(slot);
      commit_frame_ready[dirty_i] = &frame_cache.slot_ready[slot];

      if (frame_cache.slot_ready[slot]) {
        commit_frame_hit_grids++;
      }
      else {
        commit_frame_build[dirty_i] = 1;
        commit_frame_build_grids++;
      }
    }

    /* Build an O(capacity) pool of slots available to current misses. */
    Vector<int> commit_frame_available_slots;
    commit_frame_available_slots.reserve(
        std::min<int64_t>(commit_frame_misses.size(), commit_frame_capacity));

    const int cached_before = frame_cache.cached_grids;

    /* Never-used capacity costs no eviction. */
    while (frame_cache.cached_grids < frame_cache.capacity_grids &&
           commit_frame_available_slots.size() < commit_frame_misses.size())
    {
      const int slot = frame_cache.cached_grids++;
      frame_cache.slot_grids[slot] = -1;
      frame_cache.slot_ready[slot] = 0;
      commit_frame_available_slots.append(slot);
    }

    /* Existing stale slots are scanned only once, in ring order. */
    int ring_scanned = 0;
    if (frame_cache.capacity_grids > 0 &&
        commit_frame_available_slots.size() < commit_frame_misses.size())
    {
      for (int scan = 0; scan < frame_cache.capacity_grids; scan++) {
        const int candidate =
            (frame_cache.next_replace_slot + scan) % frame_cache.capacity_grids;
        ring_scanned = scan + 1;

        /* Slots >= cached_before were just added above and are already in the pool. */
        if (candidate >= cached_before) {
          continue;
        }
        if (commit_frame_slot_pinned[candidate]) {
          continue;
        }
        if (frame_cache.slot_grids[candidate] < 0) {
          continue;
        }

        commit_frame_available_slots.append(candidate);
        if (commit_frame_available_slots.size() >= commit_frame_misses.size()) {
          break;
        }
      }

      frame_cache.next_replace_slot =
          (frame_cache.next_replace_slot + ring_scanned) %
          std::max(frame_cache.capacity_grids, 1);
    }

    /* Pass 2: miss assignment is now O(1) per grid. */
    int available_i = 0;
    for (const int dirty_i : commit_frame_misses) {
      if (available_i >= commit_frame_available_slots.size()) {
        commit_frame_bypass_grids++;
        continue;
      }

      const int grid = grids[dirty_i];
      const int slot = commit_frame_available_slots[available_i++];
      const int old_grid = frame_cache.slot_grids[slot];

      if (old_grid >= 0 && old_grid < frame_cache.grids_num) {
        frame_cache.grid_slots[old_grid] = -1;
        commit_frame_replace_grids++;
      }

      frame_cache.slot_grids[slot] = grid;
      frame_cache.grid_slots[grid] = slot;
      frame_cache.slot_ready[slot] = 0;
      commit_frame_slot_pinned[slot] = 1;

      commit_frame_grids[dirty_i] = frame_cache.slot_frames(slot);
      commit_frame_ready[dirty_i] = &frame_cache.slot_ready[slot];
      commit_frame_build[dirty_i] = 1;
      commit_frame_build_grids++;
    }

    commit_frame_cached_grids = frame_cache.cached_grids;
  }

  const double commit_frame_cache_ms =
      commit_trace ? (BLI_time_now_seconds() - commit_frame_cache_start) * 1000.0 : 0.0;

  const double commit_context_ms =
      commit_trace ? (BLI_time_now_seconds() - commit_context_start) * 1000.0 : 0.0;

  const bool live_displacement_active = mmd.sculpt_displacement_enabled != 0 &&
                                        mmd.sculpt_displacement_texture != nullptr &&
                                        mmd.sculpt_displacement_strength != 0.0f;
  int commit_cached_hot_grids = 0;
  int commit_dense_sample_hot_grids = 0;
  int64_t commit_dense_sample_extra = 0;
  if (!live_displacement_active && commit_frame_cache_enabled) {
    for (const int dirty_i : commit_frame_grids.index_range()) {
      MultiresCommitBaseFrame *cached_grid = commit_frame_grids[dirty_i];
      commit_cached_hot_grids += int(cached_grid != nullptr);
      if (cached_grid != nullptr && use_exact_samples && dense_sample_bypass_enabled &&
          exact_sample_counts[dirty_i] >= dense_sample_threshold)
      {
        commit_dense_sample_hot_grids++;
        commit_dense_sample_extra += int64_t(area - exact_sample_counts[dirty_i]);
      }
    }
  }

  const float inv_side = 1.0f / float(side - 1);
  const double commit_alloc_start = commit_trace ? BLI_time_now_seconds() : 0.0;
  /* LAYER_COMMIT_COMPACT_STAGING_EXPERIMENT_V1
   * Keep only changed samples when the validated, locked journal saves at
   * least one eighth of the dense staging buffer. The solve order and float3
   * arithmetic are unchanged. All output is still staged before publication.
   * Never use this layout with an incomplete or mismatched sample journal. */
  const char *compact_env = BLI_getenv("BLENDER_SCULPT_COMPACT_COMMIT_STAGING");
  const bool compact_staging_enabled = compact_env && compact_env[0] == '1' &&
                                       compact_env[1] == '\0';
  const int64_t full_staging_bytes = full_sample_count * int64_t(sizeof(float3));
  const int64_t compact_metadata_bytes = (int64_t(grids.size()) + 1) * int64_t(sizeof(int64_t));
  const int64_t compact_saved_bytes =
      (full_sample_count - solved_sample_count) * int64_t(sizeof(float3)) - compact_metadata_bytes;
  const bool compact_staging = compact_staging_enabled && use_exact_samples &&
                               commit_dense_sample_hot_grids == 0 &&
                               compact_saved_bytes > 0 &&
                               compact_saved_bytes >= full_staging_bytes / 8;
  Array<int64_t> staged_offsets(compact_staging ? grids.size() + 1 : 0);
  if (compact_staging) {
    staged_offsets[0] = 0;
    for (const int dirty_i : grids.index_range()) {
      staged_offsets[dirty_i + 1] = staged_offsets[dirty_i] + exact_sample_counts[dirty_i];
    }
    BLI_assert(staged_offsets[grids.size()] == solved_sample_count);
  }
  Array<float3> staged(compact_staging ? solved_sample_count : full_sample_count);
  const double commit_alloc_ms =
      commit_trace ? (BLI_time_now_seconds() - commit_alloc_start) * 1000.0 : 0.0;
  if (commit_trace && compact_staging_enabled) {
    const int64_t metadata_bytes = staged_offsets.size() * sizeof(int64_t);
    std::printf("[LAYER_COMMIT_STAGING] mode=%s samples=%lld/%lld bytes=%lld "
                "metadata_bytes=%lld saved_bytes=%lld\n",
                compact_staging ? "EXACT_PACKED" : "FULL_GRID",
                static_cast<long long>(staged.size()),
                static_cast<long long>(full_sample_count),
                static_cast<long long>(staged.size() * sizeof(float3)),
                static_cast<long long>(metadata_bytes),
                static_cast<long long>((full_sample_count - staged.size()) * int64_t(sizeof(float3)) -
                                       metadata_bytes));
    std::fflush(stdout);
  }

  /* V4.3b parallel dirty-grid solve.
   *
   * Each dirty grid writes to its own staging slice and only reads persistent
   * Multires/CCG data. The shared Subdiv evaluator is already used from
   * parallel CCG evaluation elsewhere in Blender.
   *
   * Keep procedural texture displacement on the serial path for now: texture
   * evaluation may have state outside this local solve and will be handled in
   * a separate optimization pass. */
  Array<int> solve_ok(grids.size(), 0);

  const auto solve_grid = [&](const int dirty_i) -> bool {
    const int grid = grids[dirty_i];
    MutableSpan<float3> staged_grid = staged.as_mutable_span().slice(
        compact_staging ? staged_offsets[dirty_i] : int64_t(dirty_i) * area,
        compact_staging ? exact_sample_counts[dirty_i] : area);
    int staged_sample = 0;
    const Span<float3> ccg_grid = ccg.positions.as_span().slice(
        int64_t(grid) * area, area);

    MultiresCommitBaseFrame *commit_cached_frames = commit_frame_grids[dirty_i];
    uint8_t *commit_cached_ready = commit_frame_ready[dirty_i];
    if (commit_cached_frames != nullptr && commit_frame_build[dirty_i]) {
      if (commit_frame_batch_build_enabled) {
        if (!multires_sculpt_commit_build_base_frame_grid_fast(
                context, grid, side, inv_side, commit_cached_frames))
        {
          return false;
        }
        *commit_cached_ready = 1;
      }
      else {
        /* Populate a whole grid once. Dirty Multires grids are nearly dense for Grab/Elastic Grab,
         * and whole-grid construction avoids per-sample validity bookkeeping on later commits. */
        for (int cache_y = 0; cache_y < side; cache_y++) {
          for (int cache_x = 0; cache_x < side; cache_x++) {
            const int cache_i = cache_y * side + cache_x;
            const GridCoord cache_coord{
                grid, float(cache_x) * inv_side, float(cache_y) * inv_side};
            MultiresCommitBaseFrame &base_frame = commit_cached_frames[cache_i];
            float3x3 cache_tangent;
            multires_reshape_evaluate_base_mesh_limit_at_grid(
                &context, &cache_coord, base_frame.P, cache_tangent);
            if (!multires_sculpt_tangent_dual_fast(
                    cache_tangent, base_frame.dual_x, base_frame.dual_y, base_frame.dual_z))
            {
              return false;
            }
          }
        }
        *commit_cached_ready = 1;
    
      }
    }

    /* LAYER_COMMIT_CACHED_GRID_HOT_LOOP_V1
     *
     * A ready V3 cache slot already contains P + pre-solved dual tangent basis
     * for every sample of this grid. Keep that common hot path completely
     * separate from the general evaluator path:
     *   - no GridCoord construction;
     *   - no temporary tangent matrix;
     *   - no cached/uncached branch per sample;
     *   - no tangent determinant check or Gram solve.
     *
     * Live procedural displacement still uses the established general path
     * because it needs PTex coordinates for texture sampling. */
    const bool use_cached_grid_hot_loop =
        !live_displacement_active && commit_cached_frames != nullptr &&
        commit_cached_ready != nullptr && *commit_cached_ready;

    if (use_cached_grid_hot_loop) {
      const int64_t exact_sample_base = int64_t(grid) * int64_t(area);
      const bool dense_sample_grid =
          !compact_staging && use_exact_samples && dense_sample_bypass_enabled &&
          exact_sample_counts[dirty_i] >= dense_sample_threshold;

      for (int i = 0; i < area; i++) {
        if (use_exact_samples && !dense_sample_grid &&
            !ccg.sculpt_dirty_position_sample_flags[exact_sample_base + i])
        {
          continue;
        }

        const MultiresCommitBaseFrame &base_frame = commit_cached_frames[i];
        const float3 object_delta = ccg_grid[i] - base_frame.P;
        float3 delta = multires_sculpt_tangent_solve_dual(base_frame, object_delta);

        delta -= float3(base[grid].disps[i]);
        for (const MultiresSculptLayer *other : other_layers) {
          if (other->disps[grid].disps != nullptr) {
            delta -= float3(other->disps[grid].disps[i]) * other->strength;
          }
        }

        /* Keep division, validation and staging identical to the established
         * path so this optimization does not intentionally change arithmetic
         * semantics outside removal of redundant frame work. */
        delta /= layer.strength;
        if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
          return false;
        }

        staged_grid[compact_staging ? staged_sample++ : i] = delta;
      }

      return !compact_staging || staged_sample == staged_grid.size();
    }

    for (int y = 0; y < side; y++) {
      for (int x = 0; x < side; x++) {
        const int i = y * side + x;
        if (use_exact_samples &&
            !ccg.sculpt_dirty_position_sample_flags[int64_t(grid) * int64_t(area) + i])
        {
          continue;
        }
        const GridCoord coord{grid, float(x) * inv_side, float(y) * inv_side};

        float3 P;
        float3x3 tangent;
        const MultiresCommitBaseFrame *cached_base_frame = nullptr;
        if (commit_cached_frames != nullptr && commit_cached_ready != nullptr &&
            *commit_cached_ready)
        {
          cached_base_frame = &commit_cached_frames[i];
          P = cached_base_frame->P;
        }
        else {
          multires_reshape_evaluate_base_mesh_limit_at_grid(&context, &coord, P, tangent);
          const float determinant = math::determinant(tangent);
          if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f) {
            return false;
          }
        }

        const float3 object_delta = ccg_grid[i] - P;
        float3 delta;
        if (cached_base_frame != nullptr) {
          delta = multires_sculpt_tangent_solve_dual(*cached_base_frame, object_delta);
        }
        else {
          delta = use_fast_tangent_solve ?
                      multires_sculpt_tangent_solve_fast(tangent, object_delta) :
                      math::invert(tangent) * object_delta;
        }
        delta -= float3(base[grid].disps[i]);
        for (const MultiresSculptLayer *other : other_layers) {
          if (other->disps[grid].disps != nullptr) {
          delta -= float3(other->disps[grid].disps[i]) * other->strength;
        }
        }

        if (live_displacement_active) {
          const PTexCoord ptex = multires_reshape_grid_coord_to_ptex(&context, &coord);
          delta.z -= multires_texture_displacement_sample(
              mesh, *ccg.subdiv, mmd, ptex.ptex_face_index, ptex.u, ptex.v);
        }

        delta /= layer.strength;
        if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
          return false;
        }

        staged_grid[compact_staging ? staged_sample++ : i] = delta;
      }
    }

    return !compact_staging || staged_sample == staged_grid.size();
  };

  const auto solve_range = [&](const IndexRange range) {
    for (const int dirty_i : range) {
      solve_ok[dirty_i] = solve_grid(dirty_i) ? 1 : 0;
    }
  };

  /* The legacy gate excludes small grids even when millions of samples
   * need solving. Opt in to a total-work threshold and batch small grids,
   * without changing per-sample arithmetic or the publication phase. */
  const char *work_parallel_env = BLI_getenv("BLENDER_SCULPT_COMMIT_WORK_PARALLEL");
  const bool work_parallel_enabled = work_parallel_env != nullptr &&
                                     work_parallel_env[0] == '1' &&
                                     work_parallel_env[1] == '\0';
  const int64_t solve_work_samples = int64_t(grids.size()) * int64_t(area);
  const bool use_parallel_solve =
      !live_displacement_active && grids.size() >= 8 &&
      (work_parallel_enabled ? solve_work_samples >= 65536 : area >= 64);
  const int solve_grain = work_parallel_enabled ? std::max(1, 4096 / area) : 1;

  const double commit_solve_start = commit_trace ? BLI_time_now_seconds() : 0.0;
  if (use_parallel_solve) {
    threading::parallel_for(grids.index_range(), solve_grain, solve_range);
  }
  else {
    solve_range(grids.index_range());
  }
  const double commit_solve_ms =
      commit_trace ? (BLI_time_now_seconds() - commit_solve_start) * 1000.0 : 0.0;
  if (commit_trace && work_parallel_enabled) {
    std::printf("[LAYER_COMMIT_WORK_PARALLEL] parallel=%d grain=%d work_samples=%lld "
                "exact_samples=%lld live=%d solve=%.3fms\n",
                int(use_parallel_solve),
                solve_grain,
                static_cast<long long>(solve_work_samples),
                static_cast<long long>(solved_sample_count),
                int(live_displacement_active),
                commit_solve_ms);
    std::fflush(stdout);
  }

  for (const int ok : solve_ok) {
    if (ok == 0) {
      return MultiresLocalCommitResult::Failure;
    }
  }
  /* Publish only after every dirty grid has been solved and validated.
   * V4.5a sparse Sculpt Layer foundation: only dirty active grids need payload allocation. */
  /* LAYER_COMMIT_PARALLEL_PUBLISH_V1
   *
   * Each dirty grid owns an independent sparse payload and an independent
   * staging slice, so materialization/copy can be published in parallel.
   * Keep a serial fallback for A/B testing and tiny commits. */
  const double commit_publish_start = commit_trace ? BLI_time_now_seconds() : 0.0;
  Array<int> publish_ok(grids.size(), 0);

  const auto publish_range = [&](const IndexRange range) {
    for (const int dirty_i : range) {
      const int grid = grids[dirty_i];
      const Span<float3> staged_grid = staged.as_span().slice(
          compact_staging ? staged_offsets[dirty_i] : int64_t(dirty_i) * area,
          compact_staging ? exact_sample_counts[dirty_i] : area);
      int staged_sample = 0;
      float (*layer_grid)[3] = bke::multires_layers::grid_ensure(layer, grid);
      if (layer_grid == nullptr) {
        continue;
      }
      const int64_t sample_base = int64_t(grid) * int64_t(area);
      for (int i = 0; i < area; i++) {
        if (use_exact_samples &&
            !ccg.sculpt_dirty_position_sample_flags[sample_base + i])
        {
          continue;
        }
        copy_v3_v3(layer_grid[i], staged_grid[compact_staging ? staged_sample++ : i]);
      }
      BLI_assert(!compact_staging || staged_sample == staged_grid.size());
      publish_ok[dirty_i] = 1;
    }
  };

  const char *parallel_publish_env = BLI_getenv("BLENDER_SCULPT_PARALLEL_PUBLISH");
  const bool parallel_publish_enabled =
      parallel_publish_env == nullptr || parallel_publish_env[0] == '\0' ||
      parallel_publish_env[0] != '0';
  /* LAYER_COMMIT_SMALL_GRID_PUBLISH_V1: the same independent payload writes
   * also benefit from batching when low-level grids are numerous. All solves
   * and journal validation still finish before publication starts. */
  const char *small_publish_env = BLI_getenv("BLENDER_SCULPT_COMMIT_PUBLISH_SMALL_GRIDS");
  const bool small_publish_enabled = small_publish_env != nullptr &&
                                     small_publish_env[0] == '1' &&
                                     small_publish_env[1] == '\0';
  const int64_t publish_work_samples = int64_t(grids.size()) * int64_t(area);
  const bool use_parallel_publish =
      parallel_publish_enabled && grids.size() >= 8 &&
      (area >= 64 || (small_publish_enabled && publish_work_samples >= 65536));
  const int publish_grain = area >= 64 ? 32 : std::max(32, 4096 / area);
  if (commit_trace && small_publish_enabled) {
    std::printf("[LAYER_COMMIT_PUBLISH_POLICY] parallel=%d grain=%d grids=%lld "
                "area=%d work_samples=%lld\n",
                int(use_parallel_publish), publish_grain,
                static_cast<long long>(grids.size()), area,
                static_cast<long long>(publish_work_samples));
  }

  if (use_parallel_publish) {
    threading::parallel_for(grids.index_range(), publish_grain, publish_range);
  }
  else {
    publish_range(grids.index_range());
  }

  for (const int ok : publish_ok) {
    if (ok == 0) {
      return MultiresLocalCommitResult::Failure;
    }
  }

  const double commit_publish_ms =
      commit_trace ? (BLI_time_now_seconds() - commit_publish_start) * 1000.0 : 0.0;

  if (commit_trace) {
    const double commit_total_ms = (BLI_time_now_seconds() - commit_total_start) * 1000.0;
    std::printf(
        "[LAYER_COMMIT_LOCAL] grids=%d/%d samples=%lld/%lld area=%d sample_mode=%s "
        "parallel=%d publish_parallel=%d "
        "context=%.3fms alloc=%.3fms solve=%.3fms publish=%.3fms total=%.3fms\n",
        int(grids.size()),
        ccg.grids_num,
        static_cast<long long>(solved_sample_count),
        static_cast<long long>(full_sample_count),
        area,
        use_exact_samples ? "EXACT" : "GRID_FALLBACK",
        use_parallel_solve ? 1 : 0,
        use_parallel_publish ? 1 : 0,
        commit_context_ms,
        commit_alloc_ms,
        commit_solve_ms,
        commit_publish_ms,
        commit_total_ms);
  }

  if (commit_trace) {
    const double used_mib = double(commit_frame_cached_grids) * double(area) *
                            double(sizeof(MultiresCommitBaseFrame)) /
                            (1024.0 * 1024.0);
    const double capacity_mib = double(commit_frame_capacity) * double(area) *
                                double(sizeof(MultiresCommitBaseFrame)) /
                                (1024.0 * 1024.0);
    std::printf(
        "[LAYER_COMMIT_FRAME_CACHE] v=4 mode=%s reset=%d hit=%d build=%d replace=%d bypass=%d "
        "cached=%d/%d used=%.1fMiB capacity=%.1fMiB lookup=%.3fms\n",
        commit_frame_cache_enabled ? "ON" : "OFF",
        commit_frame_cache_reset ? 1 : 0,
        commit_frame_hit_grids,
        commit_frame_build_grids,
        commit_frame_replace_grids,
        commit_frame_bypass_grids,
        commit_frame_cached_grids,
        commit_frame_capacity,
        used_mib,
        capacity_mib,
        commit_frame_cache_ms);
  }

  if (commit_trace) {
    std::printf("[LAYER_COMMIT_TANGENT] mode=%s\n",
                use_fast_tangent_solve ? "FAST_GRAM" : "LEGACY_INVERSE");
    if (commit_frame_cache_enabled) {
      std::printf("[LAYER_COMMIT_CACHE_SOLVE] mode=DUAL_FAST\n");
      std::printf("[LAYER_COMMIT_FRAME_BUILD] mode=%s\n",
                  commit_frame_batch_build_enabled ? "GRID_BATCH_FAST" : "LEGACY_SAMPLE");
      std::printf("[LAYER_COMMIT_HOT_LOOP] cached=%d general=%d\n",
                  commit_cached_hot_grids,
                  int(grids.size()) - commit_cached_hot_grids);
      std::printf(
          "[LAYER_COMMIT_DENSE_SAMPLES] mode=%s threshold=97.5%% hot_grids=%d extra=%lld\n",
          dense_sample_bypass_enabled ? "ON" : "OFF",
          commit_dense_sample_hot_grids,
          static_cast<long long>(commit_dense_sample_extra));
    }
  }

  return MultiresLocalCommitResult::Success;
}

bool multires_flush_sculpt_updates(Object *object, bool *r_local_surface_preserved)
{
  if (r_local_surface_preserved) {
    *r_local_surface_preserved = false;
  }
  if (object == nullptr || object->runtime->sculpt_session == nullptr) {
    return true;
  }
  SculptSession &ss = *object->runtime->sculpt_session;
  SubdivCCG *ccg = ss.subdiv_ccg;
  if (ccg == nullptr || (!ccg->dirty.coords && !ccg->dirty.hidden && !ccg->dirty.masks)) {
    return true;
  }
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*object);
  if (!pbvh || pbvh->type() != bke::pbvh::Type::Grids || !ss.multires_modifier) {
    return false;
  }
  multires_sculpt_changes_sync(ss, *pbvh, *ccg);
  SculptChangeSet &changes = ss.change_set;

  /* Resolve the modifier which produced this session, not another Multires entry. */
  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object->modifiers) {
    if (&md == &ss.multires_modifier->modifier && md.type == eModifierType_Multires) {
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
      break;
    }
  }
  if (!mmd) {
    return false;
  }
  Mesh *mesh = id_cast<Mesh *>(object->data);
  /* POLYPAINT_BLUR_VISIBILITY_FIX_B: an Undo checkpoint may flush a pending
   * Hide operation. Visibility-only updates must never run a geometric layer
   * reshape, even when the evaluated positions happen to stay identical. */
  const char *blur_visibility_fix=BLI_getenv("BLENDER_SCULPT_POLYPAINT_BLUR_V01");
  if (blur_visibility_fix && std::strcmp(blur_visibility_fix,"1")==0 &&
      bke::facesets_visibility_rc2::enabled() && ccg->dirty.hidden && !ccg->dirty.coords)
  {
    if (changes.topology_changed || !bke::facesets_visibility_rc2::layout(*mesh,*mmd) ||
        ccg->grids_num!=mesh->corners_num || ccg->grid_size<2 ||
        ccg->grid_area!=ccg->grid_size*ccg->grid_size ||
        (!ccg->grid_hidden.is_empty() &&
         (ccg->grid_hidden.size()!=ccg->grids_num ||
          ccg->grid_hidden.group_size()!=ccg->grid_area))) { return false; }
    if (ccg->dirty.masks) {
      const BitVector<> *subset=changes.masks_full?nullptr:&changes.mask_grids;
      if (!multires_flush_mask_updates_impl(*ccg,*mesh,mmd->totlvl,subset)) { return false; }
    }
    if (!bke::facesets_visibility_rc2::import_native(*mesh,*mmd,ccg,0)) { return false; }
    ccg->dirty.hidden=false;ccg->dirty.masks=false;
    multires_sculpt_changes_clear(ss,*pbvh);
    if (r_local_surface_preserved) { *r_local_surface_preserved=true; }
    std::printf("[POLYPAINT_BLUR_VISIBILITY_FIX_B] visibility_only=1 layer_reshape=0\n");
    std::fflush(stdout);
    return true;
  }
  if (ccg->dirty.masks && !ccg->dirty.coords && !ccg->dirty.hidden) {
    const BitVector<> *mask_subset = changes.masks_full ? nullptr : &changes.mask_grids;
    if (!multires_flush_mask_updates_impl(*ccg, *mesh, mmd->totlvl, mask_subset)) {
      return false;
    }
    ccg->dirty.masks = false;
    multires_sculpt_changes_clear(ss, *pbvh);
    return true;
  }
  MultiresSculptLayer *layer = bke::multires_layers::active(*mmd);
  if (ccg->dirty.coords && layer != nullptr) {
    bke::multires_layers::weight_hot_delta_cache_invalidate(layer);
  }
  /* Hidden layers contribute neither grid nor L0 deltas to the CCG. Use native
   * reshape for that base surface without touching any stored layer payload. */
  if (!bke::multires_layers::base_has_visible_layers(*mmd)) {
    if (!multiresModifier_reshapeFromCCG(mmd->totlvl, mesh, ccg)) {
      return false;
    }
    /* Cached layer evaluation was built against the previous native base. */
    bke::multires_layers::weight_hot_delta_cache_invalidate();
  }
  else {
    const auto fail = [&]() {
      BKE_modifier_set_error(object, &mmd->modifier,
                             "Sculpt layer commit failed; pending sculpt data retained");
      return false;
    };
    if (!layer || !bke::multires_layers::compatible(*mmd, *mesh) ||
        !(layer->flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
        std::abs(layer->strength) < 1e-6f || ccg->level != mmd->sculptlvl ||
        ccg->level < 1 || ccg->level > mmd->totlvl ||
        ccg->grids_num != mesh->corners_num)
    {
      return fail();
    }
    const MultiresLocalCommitResult local_result = multires_sculpt_layer_commit_top_level_local(
        *mesh, *mmd, *layer, *ccg, changes);
    if (local_result == MultiresLocalCommitResult::Success) {
      ccg->dirty.coords = false;
      ccg->dirty.hidden = false;
      ccg->dirty.masks = false;
      multires_sculpt_changes_clear(ss, *pbvh);
      /* COMMIT_KEEP_CCG_V1: the local solver only publishes persistent layer
       * deltas. Positions, normals, masks and topology in the live CCG are
       * already the current sculpt result. Certify this path explicitly;
       * never infer it from clean dirty flags after a global/native reshape. */
      if (r_local_surface_preserved) {
        *r_local_surface_preserved = true;
      }
      return true;
    }

    const bool live_displacement_active = mmd->sculpt_displacement_enabled != 0 &&
                                          mmd->sculpt_displacement_texture != nullptr &&
                                          mmd->sculpt_displacement_strength != 0.0f;
    const char *compose_env = BLI_getenv("BLENDER_SCULPT_COMMIT_SKIP_COMPOSE");
    /* Equal-level reshape overwrites every displacement from CCG before
     * converting to tangent space. Lower-level propagation still needs the
     * composed reference. Keep live procedural displacement on its legacy path. */
    const bool skip_reference_compose = compose_env != nullptr && compose_env[0] == '1' &&
                                        compose_env[1] == '\0' &&
                                        ccg->level == mmd->totlvl &&
                                        !live_displacement_active;
    const bool compose_trace = multires_layer_commit_trace_enabled();
    const double compose_start = compose_trace ? BLI_time_now_seconds() : 0.0;
    int64_t skipped_reference_samples = 0;

    /* Reshape writes displacement AND masks. Isolate both on a temporary mesh;
     * never use the live base as conversion scratch storage. The CCG being
     * reshaped already contains the visible layer stack, so make that same
     * composed stack the reshape reference. Starting from the unlayered base
     * makes the detail-preserving reshape reinterpret layer edits against the
     * wrong reference surface and can introduce grid-like artifacts. */
    Mesh *scratch = BKE_mesh_copy_for_eval(*mesh);
    BLI_SCOPED_DEFER([&]() { BKE_id_free(nullptr, scratch); });
    Vector<const MultiresSculptLayer *> effective_layers;
    Vector<const MultiresSculptLayer *> effective_other_layers;
    effective_layers.reserve(mmd->sculpt_layers.count());
    effective_other_layers.reserve(std::max(0, mmd->sculpt_layers.count() - 1));
    for (const MultiresSculptLayer &candidate : mmd->sculpt_layers) {
      if (!bke::multires_layers::is_effective(candidate)) {
        continue;
      }
      effective_layers.append(&candidate);
      if (&candidate != layer) {
        effective_other_layers.append(&candidate);
      }
    }
    if (!bke::multires_layers::base_deltas_apply(*mmd, scratch->vert_positions_for_write())) {
      return fail();
    }
    scratch->tag_positions_changed();
    MDisps *scratch_base = static_cast<MDisps *>(CustomData_get_layer_for_write(
        &scratch->corner_data, CD_MDISPS, scratch->corners_num));
    if (!scratch_base) {
      return fail();
    }
    for (const MultiresSculptLayer *visible_layer : effective_layers) {
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        if (!scratch_base[corner].disps || !visible_layer->disps ||
            corner >= visible_layer->grids_num ||
            visible_layer->disps[corner].level != mmd->totlvl ||
            scratch_base[corner].totdisp != visible_layer->disps[corner].totdisp) {
          return fail();
        }

        /* V4.5 sparse Sculpt Layers: a null per-grid payload is an implicit
         * all-zero contribution, not an invalid layer. */
        const float (*layer_grid)[3] = visible_layer->disps[corner].disps;
        if (layer_grid == nullptr) {
          continue;
        }
        if (skip_reference_compose) {
          skipped_reference_samples += scratch_base[corner].totdisp;
          continue;
        }

        for (int i = 0; i < scratch_base[corner].totdisp; i++) {
          for (int axis = 0; axis < 3; axis++) {
            scratch_base[corner].disps[i][axis] +=
                layer_grid[i][axis] * visible_layer->strength;
          }
        }
      }
    }
    /* Live procedural displacement belongs to the evaluated reference, not to
     * recorded layer offsets. Include it during reshape and remove it below. */
    if (compose_trace) {
      std::fprintf(stdout,
                   "[LAYER_COMMIT_REFERENCE] mode=%s reshape=%d top=%d live=%d "
                   "skipped_samples=%lld prepare=%.3fms\n",
                   skip_reference_compose ? "SKIP_EQUAL_LEVEL" : "COMPOSE",
                   ccg->level,
                   mmd->totlvl,
                   int(live_displacement_active),
                   static_cast<long long>(skipped_reference_samples),
                   (BLI_time_now_seconds() - compose_start) * 1000.0);
      std::fflush(stdout);
    }
    MultiresReshapeContext live_context{};
    bool live_context_created = false;
    if (live_displacement_active) {
      if (!multires_reshape_context_create_from_ccg(&live_context, ccg, scratch, mmd->totlvl)) {
        return fail();
      }
      live_context_created = true;
    }
    BLI_SCOPED_DEFER([&]() {
      if (live_context_created) {
        multires_reshape_context_free(&live_context);
      }
    });
    const int live_side = live_context_created ? live_context.top.grid_size : 0;
    const auto live_delta = [&](const int corner, const int i) {
      if (!live_context_created) {
        return 0.0f;
      }
      const GridCoord coord{corner,
                            float(i % live_side) / float(live_side - 1),
                            float(i / live_side) / float(live_side - 1)};
      const PTexCoord ptex = multires_reshape_grid_coord_to_ptex(&live_context, &coord);
      return multires_texture_displacement_sample(
          *mesh, *ccg->subdiv, *mmd, ptex.ptex_face_index, ptex.u, ptex.v);
    };
    if (live_context_created) {
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        for (int i = 0; i < scratch_base[corner].totdisp; i++) {
          scratch_base[corner].disps[i][2] += live_delta(corner, i);
        }
      }
    }
    if (!multiresModifier_reshapeFromCCG(mmd->totlvl, scratch, ccg)) {
      return fail();
    }
    const MDisps *base = static_cast<const MDisps *>(
        CustomData_get_layer(&mesh->corner_data, CD_MDISPS));
    MDisps *result = static_cast<MDisps *>(CustomData_get_layer_for_write(
        &scratch->corner_data, CD_MDISPS, scratch->corners_num));
    if (!result) {
      return fail();
    }
    /* Stage all candidate deltas in the isolated result. Any invalid grid or
     * non-finite component aborts before a single live layer value is changed. */
    for (int corner = 0; corner < mesh->corners_num; corner++) {
      if (!result[corner].disps || result[corner].level != mmd->totlvl ||
          result[corner].totdisp != base[corner].totdisp) {
        return fail();
      }
      for (int i = 0; i < result[corner].totdisp; i++) {
        float3 delta = float3(result[corner].disps[i]) - float3(base[corner].disps[i]);
        for (const MultiresSculptLayer *other : effective_other_layers) {
          /* Sparse null payloads represent zero displacement. Descriptor
           * metadata was validated above while composing scratch_base. */
          if (other->disps[corner].disps != nullptr) {
            delta -= float3(other->disps[corner].disps[i]) * other->strength;
          }
        }
        if (live_context_created) {
          delta.z -= live_delta(corner, i);
        }
        delta /= layer->strength;
        if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
          return fail();
        }
        copy_v3_v3(result[corner].disps[i], delta);
      }
    }
    GridPaintMask *masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &scratch->corner_data, CD_GRID_PAINT_MASK, scratch->corners_num));
    if (masks) {
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        if (!masks[corner].data || masks[corner].level != mmd->totlvl) {
          return fail();
        }
        for (int i = 0; i < result[corner].totdisp; i++) {
          if (!std::isfinite(masks[corner].data[i])) {
            return fail();
          }
        }
      }
    }
    /* No fallible conversion remains beyond this point. */
    for (int corner = 0; corner < mesh->corners_num; corner++) {
      /* Both buffers are exclusively owned and have identical dimensions.
       * Transfer the validated result; scratch destruction releases the old
       * layer buffer. Keep each grid's hidden bitmap and metadata in place. */
      std::swap(layer->disps[corner].disps, result[corner].disps);
    }
    if (masks) {
      GridPaintMask *dst = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
          &mesh->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
      BLI_assert(dst != nullptr);
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        std::swap(dst[corner].data, masks[corner].data);
        std::swap(dst[corner].level, masks[corner].level);
      }
    }
  }
  ccg->dirty.coords = false;
  ccg->dirty.hidden = false;
  ccg->dirty.masks = false;
  multires_sculpt_changes_clear(ss, *pbvh);
  return true;
}

bool multires_refresh_sculpt_ccg(Object *object, MultiresModifierData *mmd)
{
  if (object == nullptr || mmd == nullptr || object->runtime->sculpt_session == nullptr) {
    return false;
  }

  SculptSession &ss = *object->runtime->sculpt_session;
  SubdivCCG *ccg = ss.subdiv_ccg;

  if (ccg == nullptr || ccg->subdiv == nullptr || ss.pbvh == nullptr) {
    return false;
  }

  Mesh *mesh = id_cast<Mesh *>(object->data);
  if (mesh == nullptr) {
    return false;
  }

  bke::subdiv::Subdiv *subdiv = ccg->subdiv;

  /* The existing evaluator contains a snapshot of the effective Sculpt
   * Layers. Re-attach it after a stack change so deleted layers are no
   * longer sampled. displacement_attach_from_multires() detaches the old
   * evaluator first. */
  bke::subdiv::displacement_attach_from_multires(
      subdiv, mesh, mmd, multires_texture_mapping_matrix(*mmd, *object));

  /* Refresh the existing CCG in-place. Do not allocate another CCG and do
   * not destroy the Sculpt PBVH. */
  if (!BKE_subdiv_ccg_evaluate_existing(*ccg, *subdiv, nullptr)) {
    return false;
  }

  bke::pbvh::Tree &pbvh = *ss.pbvh;
  const IndexRange all_nodes(pbvh.nodes_num());

  /* Position tagging also invalidates PBVH bounds, normals and draw data. */
  pbvh.tag_positions_changed(all_nodes);
  pbvh.update_bounds_grids(ccg->positions, ccg->grid_area);
  pbvh.flush_bounds_to_parents();

  /* This was an evaluation refresh caused by a layer-stack operation,
   * not a Sculpt stroke. Do not feed the whole mesh into the V3 persistent
   * sculpt-change journal. */
  pbvh.clear_changes();

  ccg->dirty.coords = false;

  return true;
}
bool multires_force_sculpt_rebuild(Object *object)
{
  if (!multires_flush_sculpt_updates(object)) {
    return false;
  }
  if (object && object->runtime->sculpt_session) {
    BKE_sculptsession_free_pbvh(*object);
  }
  return true;
}

void multires_force_external_reload(Object *object)
{
  if (!multires_flush_sculpt_updates(object)) {
    return;
  }
  for (ModifierData &md : object->modifiers) {
    if (md.type == eModifierType_Multires &&
        !reinterpret_cast<MultiresModifierData *>(&md)->sculpt_layers.is_empty()) {
      BKE_modifier_set_error(object, &md, "External reload cannot migrate Sculpt Layers yet");
      return;
    }
  }

  Mesh *mesh = BKE_mesh_from_object(object);

  CustomData_external_reload(&mesh->corner_data, &mesh->id, CD_MASK_MDISPS, mesh->corners_num);
  multires_force_sculpt_rebuild(object);
}

/* reset the multires levels to match the number of mdisps */
static int get_levels_from_disps(Object *ob)
{
  Mesh *mesh = id_cast<Mesh *>(ob->data);
  const OffsetIndices faces = mesh->faces();
  int totlvl = 0;

  const MDisps *mdisp = static_cast<const MDisps *>(
      CustomData_get_layer(&mesh->corner_data, CD_MDISPS));

  for (const int i : faces.index_range()) {
    for (const int corner : faces[i]) {
      const MDisps *md = &mdisp[corner];
      if (md->totdisp == 0) {
        continue;
      }

      while (true) {
        const int side = (1 << (totlvl - 1)) + 1;
        const int lvl_totdisp = side * side;
        if (md->totdisp == lvl_totdisp) {
          break;
        }
        if (md->totdisp < lvl_totdisp) {
          totlvl--;
        }
        else {
          totlvl++;
        }
      }

      break;
    }
  }

  return totlvl;
}

void multiresModifier_set_levels_from_disps(MultiresModifierData *mmd, Object *ob)
{
  Mesh *mesh = id_cast<Mesh *>(ob->data);
  const MDisps *mdisp;

  if (BMEditMesh *em = mesh->runtime->edit_mesh.get()) {
    mdisp = static_cast<const MDisps *>(CustomData_get_layer(&em->bm->ldata, CD_MDISPS));
  }
  else {
    mdisp = static_cast<const MDisps *>(CustomData_get_layer(&mesh->corner_data, CD_MDISPS));
  }

  if (mdisp) {
    mmd->totlvl = get_levels_from_disps(ob);
    mmd->lvl = std::min(mmd->lvl, mmd->totlvl);
    mmd->sculptlvl = std::min(mmd->sculptlvl, mmd->totlvl);
    mmd->renderlvl = std::min(mmd->renderlvl, mmd->totlvl);
  }
}

static void multires_set_tot_mdisps(Mesh *mesh, const int lvl)
{
  MDisps *mdisps = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));

  if (mdisps) {
    for (int i = 0; i < mesh->corners_num; i++, mdisps++) {
      mdisps->totdisp = multires_grid_tot[lvl];
      mdisps->level = lvl;
    }
  }
}

static void multires_copy_grid(float (*gridA)[3],
                               float (*gridB)[3],
                               const int sizeA,
                               const int sizeB)
{
  int x, y, j, skip;

  if (sizeA > sizeB) {
    skip = (sizeA - 1) / (sizeB - 1);

    for (j = 0, y = 0; y < sizeB; y++) {
      for (x = 0; x < sizeB; x++, j++) {
        copy_v3_v3(gridA[y * skip * sizeA + x * skip], gridB[j]);
      }
    }
  }
  else {
    skip = (sizeB - 1) / (sizeA - 1);

    for (j = 0, y = 0; y < sizeA; y++) {
      for (x = 0; x < sizeA; x++, j++) {
        copy_v3_v3(gridA[j], gridB[y * skip * sizeB + x * skip]);
      }
    }
  }
}

/* Reallocate gpm->data at a lower resolution and copy values over
 * from the original high-resolution data */
static void multires_grid_paint_mask_downsample(GridPaintMask *gpm, const int level)
{
  if (level < gpm->level) {
    const int gridsize = CCG_grid_size(level);
    float *data = MEM_new_array_zeroed<float>(size_t(square_i(gridsize)), __func__);

    for (int y = 0; y < gridsize; y++) {
      for (int x = 0; x < gridsize; x++) {
        data[y * gridsize + x] = paint_grid_paint_mask(gpm, level, x, y);
      }
    }

    MEM_delete(gpm->data);
    gpm->data = data;
    gpm->level = level;
  }
}

static void multires_del_higher(MultiresModifierData *mmd, Object *ob, const int lvl)
{
  Mesh *mesh = id_cast<Mesh *>(ob->data);
  const OffsetIndices faces = mesh->faces();
  const int levels = mmd->totlvl - lvl;
  MDisps *mdisps;
  GridPaintMask *gpm;

  multires_set_tot_mdisps(mesh, mmd->totlvl);
  multiresModifier_ensure_external_read(mesh, mmd);
  mdisps = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));
  gpm = static_cast<GridPaintMask *>(
      CustomData_get_layer_for_write(&mesh->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));

  multires_force_sculpt_rebuild(ob);

  if (mdisps && levels > 0) {
    if (lvl > 0) {
      const int nsize = multires_side_tot[lvl];
      const int hsize = multires_side_tot[mmd->totlvl];

      for (const int i : faces.index_range()) {
        for (const int corner : faces[i]) {
          MDisps *mdisp = &mdisps[corner];
          const int totdisp = multires_grid_tot[lvl];

          float (*disps)[3] = MEM_new_array_zeroed<float[3]>(totdisp, "multires disps");

          if (mdisp->disps != nullptr) {
            float (*ndisps)[3] = disps;
            float (*hdisps)[3] = mdisp->disps;
            multires_copy_grid(ndisps, hdisps, nsize, hsize);
            MEM_delete(mdisp->disps);
          }
          /* FACESETS_RC7_HIDDEN_FIX_A: visibility also exists on zero-displacement grids. */
          if (mdisp->hidden) {
            BLI_bitmap *gh = multires_mdisps_downsample_hidden(mdisp->hidden, mdisp->level, lvl);
            MEM_delete(mdisp->hidden);
            mdisp->hidden = gh;
          }

          mdisp->disps = disps;
          mdisp->totdisp = totdisp;
          mdisp->level = lvl;

          if (gpm) {
            multires_grid_paint_mask_downsample(&gpm[corner], lvl);
          }
        }
      }
    }
    else {
      multires_customdata_delete(mesh);
    }
  }

  multires_set_tot_level(ob, mmd, lvl);
}

/* Validate every descriptor and every surviving sample before freeing any grid.
 * Deleted detail does not need a high-resolution scan. Null layer payloads are
 * implicit zero grids and must remain sparse after truncation. */
static bool multires_delete_higher_validate(const MultiresModifierData &mmd,
                                           const Mesh &mesh,
                                           const int level)
{
  if (mmd.totlvl < 1 || mmd.totlvl > 13 || level < 0 || level >= mmd.totlvl ||
      !bke::multires_layers::compatible(mmd, mesh))
  {
    return false;
  }
  if (mmd.sculpt_color_grids) {
    if (mmd.sculpt_color_grids_num != mesh.corners_num || mmd.sculpt_color_level < 1 ||
        mmd.sculpt_color_level > 13)
    {
      return false;
    }
    const int area = multires_grid_tot[mmd.sculpt_color_level];
    for (int corner = 0; corner < mesh.corners_num; corner++) {
      const MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
      if (grid.colors && (grid.level != mmd.sculpt_color_level || grid.totcolor != area)) {
        return false;
      }
    }
  }
  for (const MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
    if (!attribute.grids) continue;
    if (attribute.grids_num != mesh.corners_num || attribute.level < 1 || attribute.level > 13) {
      return false;
    }
    const int area = multires_grid_tot[attribute.level];
    for (int corner = 0; corner < mesh.corners_num; corner++) {
      const MultiresColorGrid &grid = attribute.grids[corner];
      if (grid.colors && (grid.level != attribute.level || grid.totcolor != area)) {
        return false;
      }
    }
  }
  if (mmd.sculpt_face_set_grids) {
    if (mmd.sculpt_face_set_grids_num != mesh.corners_num || mmd.sculpt_face_set_level < 1 ||
        mmd.sculpt_face_set_level > 13)
    {
      return false;
    }
    const int area = multires_grid_tot[mmd.sculpt_face_set_level];
    for (int corner = 0; corner < mesh.corners_num; corner++) {
      const MultiresFaceSetGrid &grid = mmd.sculpt_face_set_grids[corner];
      if (grid.ids && (grid.level != mmd.sculpt_face_set_level || grid.totid != area)) {
        return false;
      }
    }
  }
  if (level == 0) {
    return true;
  }
  const int old_side = multires_side_tot[mmd.totlvl];
  const int new_side = multires_side_tot[level];
  const int step = (old_side - 1) / (new_side - 1);
  const auto finite_samples = [&](const MDisps &grid) {
    if (!grid.disps) {
      return true;
    }
    for (int y = 0; y < new_side; y++) {
      for (int x = 0; x < new_side; x++) {
        const float *sample = grid.disps[y * step * old_side + x * step];
        if (!std::isfinite(sample[0]) || !std::isfinite(sample[1]) ||
            !std::isfinite(sample[2]))
        {
          return false;
        }
      }
    }
    return true;
  };
  const MDisps *base = static_cast<const MDisps *>(
      CustomData_get_layer(&mesh.corner_data, CD_MDISPS));
  for (int corner = 0; corner < mesh.corners_num; corner++) {
    if (!finite_samples(base[corner])) {
      return false;
    }
    for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
      if (!finite_samples(layer.disps[corner])) {
        return false;
      }
    }
  }
  return true;
}

static void multires_sculpt_layers_delete_higher(MultiresModifierData &mmd, const int level)
{
  bke::multires_layers::weight_hot_delta_cache_invalidate();
  const int old_side = multires_side_tot[mmd.totlvl];
  const int new_side = level > 0 ? multires_side_tot[level] : 0;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    for (int corner = 0; corner < layer.grids_num; corner++) {
      MDisps &grid = layer.disps[corner];
      float (*disps)[3] = nullptr;
      BLI_bitmap *hidden = nullptr;
      if (level > 0) {
        if (grid.disps) {
          disps = MEM_new_array_uninitialized<float[3]>(multires_grid_tot[level],
                                                      "truncated sculpt layer grid");
          multires_copy_grid(disps, grid.disps, new_side, old_side);
        }
        if (grid.hidden) {
          hidden = multires_mdisps_downsample_hidden(grid.hidden, grid.level, level);
        }
      }
      MEM_SAFE_DELETE(grid.disps);
      MEM_SAFE_DELETE(grid.hidden);
      grid.disps = disps;
      grid.hidden = hidden;
      grid.level = level;
      grid.totdisp = level > 0 ? multires_grid_tot[level] : 0;
    }
    if (level == 0) {
      MEM_SAFE_DELETE(layer.disps);
      layer.grids_num = 0;
    }
    layer.level = level;
    /* Keep L0 deltas, identity, Strength, visibility, groups and REC. Polypaint
     * and Face Set master grids retain their own stored resolution: the base
     * topology and its corner indices have not changed. */
  }
}

void multiresModifier_del_levels(MultiresModifierData *mmd,
                                 Scene *scene,
                                 Object *ob,
                                 const int direction)
{
  Mesh *mesh = BKE_mesh_from_object(ob);
  const int lvl = multires_get_level(scene, ob, mmd, false, true);
  if (direction != 1 || lvl < 0 || lvl >= mmd->totlvl) {
    BKE_modifier_set_error(ob, &mmd->modifier, "No higher Multires levels to delete");
    return;
  }
  /* Layer stacks belong to objects while native displacement belongs to the
   * mesh. Avoid truncating another object's stack through a shared mesh. */
  if (ID_REAL_USERS(&mesh->id) > 1) {
    BKE_modifier_set_error(ob, &mmd->modifier,
                           "Make the mesh single-user before deleting higher levels");
    return;
  }
  int multires_modifiers = 0;
  for (const ModifierData &md : ob->modifiers) {
    multires_modifiers += md.type == eModifierType_Multires;
  }
  if (multires_modifiers != 1) {
    BKE_modifier_set_error(ob, &mmd->modifier,
                           "Deleting higher levels requires a single Multires modifier");
    return;
  }
  if (!multires_flush_sculpt_updates(ob)) {
    BKE_modifier_set_error(ob, &mmd->modifier,
                           "Cannot delete higher levels while sculpt edits cannot be committed");
    return;
  }
  /* L0 topology edits currently do not remap companion masters. Refuse this
   * transition explicitly until a separate base-attribute migration exists.
   * Check after the commit, which may materialize companion storage. */
  if (lvl == 0 && (mmd->sculpt_color_grids || mmd->sculpt_face_set_grids ||
                   !mmd->sculpt_color_attributes.is_empty())) {
    BKE_modifier_set_error(ob, &mmd->modifier,
                           "Keep at least level 1 while Multires Polypaint or Face Sets are stored");
    return;
  }
  multiresModifier_ensure_external_read(mesh, mmd);
  if (!multires_delete_higher_validate(*mmd, *mesh, lvl)) {
    BKE_modifier_set_error(ob, &mmd->modifier,
                           "Invalid Multires or Sculpt Layer grids; higher levels retained");
    return;
  }
  if (!multires_force_sculpt_rebuild(ob)) {
    BKE_modifier_set_error(ob, &mmd->modifier,
                           "Cannot rebuild sculpt data; higher levels retained");
    return;
  }
  /* Validation is complete and the live PBVH no longer references the payloads.
   * Publish one smaller grid at a time to bound temporary allocation. */
  multires_sculpt_layers_delete_higher(*mmd, lvl);
  multires_del_higher(mmd, ob, lvl);
  /* The native helper leaves viewport level unchanged in Sculpt Mode. It must
   * not refer to a deleted level when the object returns to Object Mode. */
  mmd->lvl = std::min<int>(mmd->lvl, lvl);
}

void multires_stitch_grids(Object *ob)
{
  if (ob == nullptr) {
    return;
  }
  SculptSession *sculpt_session = ob->runtime->sculpt_session;
  if (sculpt_session == nullptr) {
    return;
  }
  SubdivCCG *subdiv_ccg = sculpt_session->subdiv_ccg;
  if (subdiv_ccg == nullptr) {
    return;
  }
  BLI_assert(bke::object::pbvh_get(*ob) &&
             bke::object::pbvh_get(*ob)->type() == bke::pbvh::Type::Grids);
  BKE_subdiv_ccg_average_stitch_faces(*subdiv_ccg, IndexMask(subdiv_ccg->faces.size()));
}

void old_mdisps_bilinear(float out[3], float (*disps)[3], const int st, float u, float v)
{
  const int st_max = st - 1;
  float d[4][3], d2[2][3];

  if (!disps || isnan(u) || isnan(v)) {
    return;
  }

  if (u < 0) {
    u = 0;
  }
  else if (u >= st) {
    u = st_max;
  }
  if (v < 0) {
    v = 0;
  }
  else if (v >= st) {
    v = st_max;
  }

  const int x = floor(u);
  const int y = floor(v);
  int x2 = x + 1;
  int y2 = y + 1;

  if (x2 >= st) {
    x2 = st_max;
  }
  if (y2 >= st) {
    y2 = st_max;
  }

  const float urat = u - x;
  const float vrat = v - y;
  const float uopp = 1 - urat;

  mul_v3_v3fl(d[0], disps[y * st + x], uopp);
  mul_v3_v3fl(d[1], disps[y * st + x2], urat);
  mul_v3_v3fl(d[2], disps[y2 * st + x], uopp);
  mul_v3_v3fl(d[3], disps[y2 * st + x2], urat);

  add_v3_v3v3(d2[0], d[0], d[1]);
  add_v3_v3v3(d2[1], d[2], d[3]);
  mul_v3_fl(d2[0], 1 - vrat);
  mul_v3_fl(d2[1], vrat);

  add_v3_v3v3(out, d2[0], d2[1]);
}

void multiresModifier_sync_levels_ex(Object *ob_dst,
                                     const MultiresModifierData *mmd_src,
                                     MultiresModifierData *mmd_dst)
{
  if (mmd_src->totlvl == mmd_dst->totlvl) {
    return;
  }

  if (mmd_src->totlvl > mmd_dst->totlvl) {
    multiresModifier_subdivide_to_level(
        ob_dst, mmd_dst, mmd_src->totlvl, MultiresSubdivideModeType::CatmullClark);
  }
  else {
    multires_del_higher(mmd_dst, ob_dst, mmd_src->totlvl);
  }
}

static void multires_sync_levels(Scene *scene, Object *ob_src, Object *ob_dst)
{
  MultiresModifierData *mmd_src = get_multires_modifier(scene, ob_src, true);
  MultiresModifierData *mmd_dst = get_multires_modifier(scene, ob_dst, true);

  if (!mmd_src) {
    /* NOTE(@sergey): object could have MDISP even when there is no multires modifier
     * this could lead to troubles due to I've got no idea how mdisp could be
     * up-sampled correct without modifier data. Just remove mdisps if no multires present. */
    multires_customdata_delete(id_cast<Mesh *>(ob_src->data));
  }

  if (mmd_src && mmd_dst) {
    multiresModifier_sync_levels_ex(ob_dst, mmd_src, mmd_dst);
  }
}

static void multires_apply_uniform_scale(Object *object, const float scale)
{
  Mesh *mesh = id_cast<Mesh *>(object->data);
  MDisps *mdisps = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));
  for (int i = 0; i < mesh->corners_num; i++) {
    MDisps *grid = &mdisps[i];
    for (int j = 0; j < grid->totdisp; j++) {
      mul_v3_fl(grid->disps[j], scale);
    }
  }
}

static void multires_apply_smat(Depsgraph * /*depsgraph*/,
                                Scene *scene,
                                Object *object,
                                const float smat[3][3])
{
  const MultiresModifierData *mmd = get_multires_modifier(scene, object, true);
  if (mmd == nullptr || mmd->totlvl == 0) {
    return;
  }
  /* Make sure layer present. */
  Mesh *mesh = id_cast<Mesh *>(object->data);
  multiresModifier_ensure_external_read(mesh, mmd);
  if (!CustomData_get_layer(&mesh->corner_data, CD_MDISPS)) {
    return;
  }
  if (is_uniform_scaled_m3(smat)) {
    const float scale = mat3_to_scale(smat);
    multires_apply_uniform_scale(object, scale);
  }
  else {
    /* TODO(@sergey): This branch of code actually requires more work to
     * preserve all the details. */
    const float scale = mat3_to_scale(smat);
    multires_apply_uniform_scale(object, scale);
  }
}

int multires_mdisp_corners(const MDisps *s)
{
  int lvl = 13;

  while (lvl > 0) {
    const int side = (1 << (lvl - 1)) + 1;
    if ((s->totdisp % (side * side)) == 0) {
      return s->totdisp / (side * side);
    }
    lvl--;
  }

  return 0;
}

void multiresModifier_scale_disp(Depsgraph *depsgraph, Scene *scene, Object *ob)
{
  float smat[3][3];

  /* object's scale matrix */
  BKE_object_scale_to_mat3(ob, smat);

  multires_apply_smat(depsgraph, scene, ob, smat);
}

void multiresModifier_prepare_join(Depsgraph *depsgraph, Scene *scene, Object *ob, Object *to_ob)
{
  float smat[3][3], tmat[3][3], mat[3][3];
  multires_sync_levels(scene, to_ob, ob);

  /* construct scale matrix for displacement */
  BKE_object_scale_to_mat3(to_ob, tmat);
  invert_m3(tmat);
  BKE_object_scale_to_mat3(ob, smat);
  mul_m3_m3m3(mat, smat, tmat);

  multires_apply_smat(depsgraph, scene, ob, mat);
}

void multires_topology_changed(Mesh *mesh)
{
  CustomData_external_read(&mesh->corner_data, "", &mesh->id, CD_MASK_MDISPS, mesh->corners_num);
  MDisps *mdisp = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));

  if (!mdisp) {
    return;
  }

  MDisps *cur = mdisp;
  int grid = 0;
  for (int i = 0; i < mesh->corners_num; i++, cur++) {
    if (cur->totdisp) {
      grid = mdisp->totdisp;

      break;
    }
  }

  for (int i = 0; i < mesh->corners_num; i++, mdisp++) {
    /* allocate memory for mdisp, the whole disp layer would be erased otherwise */
    if (!mdisp->totdisp || !mdisp->disps) {
      if (grid) {
        mdisp->totdisp = grid;
        mdisp->disps = MEM_new_array_zeroed<float[3]>(mdisp->totdisp, "mdisp topology");
      }

      continue;
    }
  }
}

void multires_ensure_external_read(Mesh *mesh, const int top_level)
{
  if (!CustomData_external_test(&mesh->corner_data, CD_MDISPS)) {
    return;
  }

  /* Modify the data array from the original mesh, not the evaluated mesh.
   * When multiple objects share the same mesh, this can lead to memory leaks. */
  MDisps *mdisps = const_cast<MDisps *>(
      static_cast<const MDisps *>(CustomData_get_layer(&mesh->corner_data, CD_MDISPS)));
  if (mdisps == nullptr) {
    mdisps = static_cast<MDisps *>(
        CustomData_add_layer(&mesh->corner_data, CD_MDISPS, CD_SET_DEFAULT, mesh->corners_num));
  }

  const int totloop = mesh->corners_num;

  for (int i = 0; i < totloop; ++i) {
    if (mdisps[i].level != top_level) {
      MEM_SAFE_DELETE(mdisps[i].disps);
    }

    /* NOTE: CustomData_external_read will take care of allocation of displacement vectors if
     * they are missing. */

    const int totdisp = multires_grid_tot[top_level];
    mdisps[i].totdisp = totdisp;
    mdisps[i].level = top_level;
  }

  CustomData_external_read(&mesh->corner_data, "", &mesh->id, CD_MASK_MDISPS, mesh->corners_num);
}
void multiresModifier_ensure_external_read(Mesh *mesh, const MultiresModifierData *mmd)
{
  multires_ensure_external_read(mesh, mmd->totlvl);
}

}  // namespace blender
