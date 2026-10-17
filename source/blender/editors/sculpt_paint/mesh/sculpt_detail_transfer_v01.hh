/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BKE_lib_id.hh"

namespace blender::ed::sculpt_paint {

static bool detail_transfer_import_poll_v01(bContext *C)
{
  const Object *object = CTX_data_active_object(C);
  return object && object->type == OB_MESH && object->mode == OB_MODE_OBJECT;
}

static wmOperatorStatus detail_transfer_import_exec_v01(bContext *C, wmOperator *op)
{
  const char *flag = BLI_getenv("BLENDER_SCULPT_DETAIL_TRANSFER_V01");
  if (!flag || flag[0] != '1' || flag[1] != '\0') {
    BKE_report(op->reports, RPT_ERROR, "Use the Detail Transfer V01 launch profile");
    return OPERATOR_CANCELLED;
  }
  Object &object = *CTX_data_active_object(C);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  auto *mmd = reinterpret_cast<MultiresModifierData *>(
      BKE_modifiers_findby_type(&object, eModifierType_Multires));
  if (!mmd || object.id.lib || object.id.override_library || mesh.id.lib ||
      mesh.id.us != 1 || mesh.key || object.runtime->sculpt_session ||
      object.modifiers.count() != 1 || mmd->totlvl < 1 || mmd->totlvl > 13 ||
      mmd->lvl != mmd->totlvl || mmd->sculpt_displacement_enabled ||
      mmd->sculpt_shrinkwrap_enabled ||
      CustomData_external_test(&mesh.corner_data, CD_MDISPS) ||
      !(U.uiflag & USER_GLOBALUNDO) || U.undosteps <= 0)
  {
    BKE_report(op->reports, RPT_ERROR,
        "Use Object Mode, a local single-user mesh, Global Undo, one Multires modifier at "
        "maximum viewport level, no shape keys, external grids or live displacement/shrinkwrap");
    return OPERATOR_CANCELLED;
  }
  char preview_name[MAX_ID_NAME], layer_name[64];
  RNA_string_get(op->ptr, "preview_object", preview_name);
  RNA_string_get(op->ptr, "layer_name", layer_name);
  Object *preview = reinterpret_cast<Object *>(
      BKE_libblock_find_name(CTX_data_main(C), ID_OB, preview_name));
  if (!preview || preview == &object || preview->type != OB_MESH ||
      !preview->modifiers.is_empty() || preview->mode != OB_MODE_OBJECT)
  {
    BKE_report(op->reports, RPT_ERROR, "Choose an unmodified Detail Transfer preview mesh");
    return OPERATOR_CANCELLED;
  }
  for (int column = 0; column < 4; column++) {
    for (int row = 0; row < 4; row++) {
      const float a = object.object_to_world()[column][row];
      const float b = preview->object_to_world()[column][row];
      if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > 1e-6f) {
        BKE_report(op->reports, RPT_ERROR, "Target/preview transforms changed; rebuild the preview");
        return OPERATOR_CANCELLED;
      }
    }
  }
  const Mesh &incoming = *id_cast<const Mesh *>(preview->data);
  if (incoming.verts_num <= 0 || incoming.verts_num > 1000000) {
    BKE_report(op->reports, RPT_ERROR, "V01 preview limit is one million evaluated vertices");
    return OPERATOR_CANCELLED;
  }
  const VArraySpan<float3> origin = *incoming.attributes().lookup<float3>(
      "detail_transfer_origin", bke::AttrDomain::Point);
  const VArraySpan<float3> deltas = *incoming.attributes().lookup<float3>(
      "detail_transfer_delta", bke::AttrDomain::Point);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  const Object *evaluated = DEG_get_evaluated(depsgraph, &object);
  const Mesh *current = evaluated ? BKE_object_get_evaluated_mesh(evaluated) : nullptr;
  if (!current || current->verts_num != incoming.verts_num ||
      current->faces_num != incoming.faces_num || current->corners_num != incoming.corners_num ||
      origin.size() != incoming.verts_num || deltas.size() != incoming.verts_num)
  {
    BKE_report(op->reports, RPT_ERROR, "Target/preview layout changed or attributes are missing");
    return OPERATOR_CANCELLED;
  }
  const Span<int> current_corners = current->corner_verts();
  const Span<int> incoming_corners = incoming.corner_verts();
  for (const int i : current_corners.index_range()) {
    if (current_corners[i] != incoming_corners[i]) {
      BKE_report(op->reports, RPT_ERROR, "Target/preview vertex order differs; rebuild the preview");
      return OPERATOR_CANCELLED;
    }
  }
  const OffsetIndices<int> current_faces = current->faces();
  const OffsetIndices<int> incoming_faces = incoming.faces();
  for (const int i : current_faces.index_range()) {
    if (current_faces[i].start() != incoming_faces[i].start() ||
        current_faces[i].size() != incoming_faces[i].size()) {
      BKE_report(op->reports, RPT_ERROR, "Target/preview polygon layout differs");
      return OPERATOR_CANCELLED;
    }
  }
  const Span<float3> current_positions = current->vert_positions();
  const Span<float3> preview_positions = incoming.vert_positions();
  for (const int i : current_positions.index_range()) {
    for (int axis = 0; axis < 3; axis++) {
      const float p = current_positions[i][axis], old = origin[i][axis];
      const float delta = deltas[i][axis], shown = preview_positions[i][axis];
      const float tolerance = 2e-6f * std::max(1.0f, std::abs(p));
      if (!std::isfinite(p) || !std::isfinite(old) || !std::isfinite(delta) ||
          !std::isfinite(shown) || std::abs(p - old) > tolerance ||
          std::abs(shown - (old + delta)) > tolerance)
      {
        BKE_report(op->reports, RPT_ERROR,
                   "Target or preview positions changed; rebuild the frozen preview");
        return OPERATOR_CANCELLED;
      }
    }
  }
  UndoStack *stack = ED_undo_stack_get();
  if (!stack || stack->step_init || stack->group_level != 0 ||
      !ED_undo_is_memfile_compatible(C)) {
    BKE_report(op->reports, RPT_ERROR, "Finish the active Undo transaction first");
    return OPERATOR_CANCELLED;
  }
  const UndoStep *previous = stack->step_active;
  ED_undo_push(C, "Before Detail Transfer Layer");
  if (!stack->step_active || stack->step_active == previous ||
      stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE) {
    BKE_report(op->reports, RPT_ERROR, "Could not establish the pre-transfer Undo checkpoint");
    return OPERATOR_CANCELLED;
  }
  const char *error = bke::multires_layers::detail_delta_import_to_new_layer(
      object, incoming, layer_name);
  if (error) {
    BKE_report(op->reports, RPT_ERROR, error);
    return OPERATOR_CANCELLED;
  }
  DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, &object);
  BKE_report(op->reports, RPT_INFO, "Detail transferred to a new Sculpt Layer; previous payloads retained");
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_detail_transfer_import_v01(wmOperatorType *ot)
{
  ot->name = "Import Detail Transfer as New Layer";
  ot->idname = "SCULPT_OT_multires_detail_transfer_import_v01";
  ot->description = "Import frozen preview translations into a new additive Multires layer";
  ot->poll = detail_transfer_import_poll_v01;
  ot->exec = detail_transfer_import_exec_v01;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_string(ot->srna, "preview_object", "", MAX_ID_NAME, "Preview", "Frozen preview object");
  RNA_def_string(ot->srna, "layer_name", "Transferred Detail", 64, "Layer", "New layer name");
}

}  // namespace blender::ed::sculpt_paint
