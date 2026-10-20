/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BLI_memory_utils.hh"

namespace blender::bke::multires_layers {

/* DETAIL_TRANSFER_V01: testable conversion of an object-space translation into
 * the destination's native Multires tangent frame. No subtraction of large
 * absolute surface positions and no change to existing layer payloads. */
static bool detail_transfer_tangent_delta_v01(const float3x3 &frame,
                                            const float3 &movement,
                                            float3 &result)
{
  if (!std::isfinite(movement.x) || !std::isfinite(movement.y) ||
      !std::isfinite(movement.z)) {
    return false;
  }
  if (movement.x == 0.0f && movement.y == 0.0f && movement.z == 0.0f) {
    result = float3(0.0f);
    return true;
  }
  const float determinant = math::determinant(frame);
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f) {
    return false;
  }
  result = math::invert(frame) * movement;
  return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z);
}

const char *detail_delta_import_to_new_layer(Object &object,
                                            const Mesh &delta_mesh,
                                            const char *name)
{
  auto *mmd = reinterpret_cast<MultiresModifierData *>(
      BKE_modifiers_findby_type(&object, eModifierType_Multires));
  Mesh *mesh = BKE_mesh_from_object(&object);
  if (!mmd || !mesh || mmd->totlvl < 1 || mmd->totlvl > 13 ||
      mmd->lvl != mmd->totlvl || !compatible(*mmd, *mesh)) {
    return "Use valid Multires grids with the viewport at the maximum level";
  }
  const VArraySpan<float3> deltas = *delta_mesh.attributes().lookup<float3>(
      "detail_transfer_delta", AttrDomain::Point);
  if (deltas.is_empty() || deltas.size() != delta_mesh.verts_num) {
    return "Preview has no point-vector detail_transfer_delta attribute";
  }
  for (const float3 &delta : deltas) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
      return "Non-finite preview displacement; no layer added";
    }
  }
  /* COW scratch only: native base displacements, masks and cage stay untouched. */
  Mesh *scratch = BKE_mesh_copy_for_eval(*mesh);
  BLI_SCOPED_DEFER([&]() { BKE_id_free(nullptr, scratch); });
  if (!base_deltas_apply(*mmd, scratch->vert_positions_for_write())) {
    return "Could not construct the destination cage frame; no layer added";
  }
  scratch->tag_positions_changed();
  Object work_object = dna::shallow_copy(object);
  work_object.data = &scratch->id;
  MultiresModifierData work_mmd = dna::shallow_copy(*mmd);
  work_mmd.sculpt_layers = {};
  MultiresReshapeContext context{};
  if (!multires_reshape_context_create_from_modifier(
          &context, &work_object, &work_mmd, mmd->totlvl)) {
    return "Could not initialize the native Multires correspondence";
  }
  BLI_SCOPED_DEFER([&]() { multires_reshape_context_free(&context); });
  const int side = subdiv::grid_size_from_level(mmd->totlvl);
  const int area = side * side;
  if (context.num_grids != mesh->corners_num || !context.mdisps) {
    return "Unexpected native grid layout; no layer added";
  }
  for (int grid = 0; grid < context.num_grids; grid++) {
    const MDisps &item = context.mdisps[grid];
    if (!item.disps || item.level != mmd->totlvl || item.totdisp != area) {
      return "Invalid scratch displacement layout; no layer added";
    }
  }
  /* Reuse Blender's exact ToMesh-vertex -> CCG-grid scatter, including all seam
   * duplicates. These are vectors, not final coordinates: never call the
   * absolute-position/tangent conversion or generic Reshape on the live mesh. */
  if (!multires_reshape_assign_final_coords_from_vertcos(&context, deltas)) {
    return "Preview vertex order/count does not match native Multires output";
  }

  MultiresModifierData staged{};
  MultiresSculptLayer *layer = add(staged, name, mmd->totlvl, mesh->corners_num);
  if (!layer) {
    return "Could not stage an empty Sculpt Layer";
  }
  BLI_SCOPED_DEFER([&]() { clear(staged); });
  bool any_change = false;
  for (int grid = 0; grid < context.num_grids; grid++) {
    const float (*movements)[3] = context.mdisps[grid].disps;
    for (int y = 0; y < side; y++) {
      for (int x = 0; x < side; x++) {
        const int index = y * side + x;
        const float3 movement(movements[index]);
        if (movement.x == 0.0f && movement.y == 0.0f && movement.z == 0.0f) {
          continue;
        }
        const GridCoord coord{grid, float(x) / float(side - 1), float(y) / float(side - 1)};
        float3 base_position;
        float3x3 tangent;
        multires_reshape_evaluate_base_mesh_limit_at_grid(&context, &coord, base_position, tangent);
        float3 delta;
        if (!detail_transfer_tangent_delta_v01(tangent, movement, delta)) {
          return "Singular or non-finite target tangent frame; no layer added";
        }
        float (*payload)[3] = grid_ensure(*layer, grid);
        if (!payload) {
          return "Could not stage the new layer payload";
        }
        copy_v3_v3(payload[index], delta);
        any_change = true;
      }
    }
  }
  if (!any_change) {
    return "Preview contains no non-zero detail; no layer added";
  }

  /* Atomic publication after all scatter/conversion/finite checks. No live
   * base, mask, Face Set, color or previous layer numeric storage is written. */
  BLI_remlink(&staged.sculpt_layers, layer);
  BLI_addtail(&mmd->sculpt_layers, layer);
  BLI_uniquename(&mmd->sculpt_layers, layer, "Transferred Detail", '.',
                offsetof(MultiresSculptLayer, name), sizeof(layer->name));
  mmd->sculpt_layer_active = mmd->sculpt_layers.count() - 1;
  mmd->sculpt_layer_recording = 0;
  /* Same structural identity setup as the ordinary Add Sculpt Layer operator;
   * this creates metadata attributes only, without changing cage coordinates. */
  base_ensure_vertex_ids(*mesh);
  ensure_corner_ids(*mmd, *mesh);
  weight_hot_delta_cache_invalidate();
  payload_index_invalidate();
  return nullptr;
}

}  // namespace blender::bke::multires_layers
