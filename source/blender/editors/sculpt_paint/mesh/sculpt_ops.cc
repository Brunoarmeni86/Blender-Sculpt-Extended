/* SPDX-FileCopyrightText: 2006 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Implements the Sculpt Mode tools.
 */

#include "BLI_color.hh"
#include "BLI_bit_vector.hh"
#include "BLI_index_mask.hh"
#include "BLI_path_utils.hh"
#include "BLI_time.h"
#include <cstdio>
#include <cstdint>
#include "MEM_guardedalloc.h"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_brush_types.h"
#include "DNA_listBase.h"
#include "DNA_modifier_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_ccg.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mirror.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_undo_system.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subdiv.hh"

#include "DEG_depsgraph.hh"

#include "IMB_colormanagement.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_image.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_undo.hh"

#include "../paint_intern.hh"
#include "mesh_brush_common.hh"
#include "paint_mask.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_dyntopo.hh"
#include "sculpt_flood_fill.hh"
#include "sculpt_intern.hh"
#include "sculpt_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "bmesh.hh"

#include <cmath>
#include <cstring>
#include <string>

namespace blender::ed::sculpt_paint {


static MultiresModifierData *sculpt_multires_layer_modifier(Object &ob)
{
  if (ob.runtime->sculpt_session && ob.runtime->sculpt_session->multires_modifier) {
    return ob.runtime->sculpt_session->multires_modifier;
  }
  return reinterpret_cast<MultiresModifierData *>(BKE_modifiers_findby_type(
      &ob, eModifierType_Multires));
}

/* Read-only diagnostic. No allocation, flush, evaluation, tagging or Undo push. */
struct MultiresAuditHash {
  uint64_t value = UINT64_C(14695981039346656037);
  uint64_t words = 0;
  uint64_t invalid = 0;

  void word(const uint32_t bits)
  {
    value = (value ^ bits) * UINT64_C(1099511628211);
    words++;
  }
  void scalar(const float component)
  {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(component));
    std::memcpy(&bits, &component, sizeof(bits));
    word(bits);
    invalid += !std::isfinite(component);
  }
  void text(const char *value)
  {
    for (; *value; value++) {
      word(uint32_t(static_cast<unsigned char>(*value)));
    }
    word(0);
  }
};

static MultiresAuditHash multires_audit_disps(const MDisps *data, const int count)
{
  MultiresAuditHash hash;
  hash.word(uint32_t(count));
  hash.word(data != nullptr);
  if (!data) {
    hash.invalid += count != 0;
    return hash;
  }
  for (int grid = 0; grid < count; grid++) {
    const MDisps &item = data[grid];
    hash.word(uint32_t(item.level));
    hash.word(uint32_t(item.totdisp));
    hash.word(item.disps != nullptr);
    if (item.totdisp == 0 && item.disps == nullptr) {
      continue;
    }
    if (item.level < 1 || item.level > 13) {
      hash.invalid++;
      continue;
    }
    const int side = (1 << (item.level - 1)) + 1;
    if (item.totdisp != side * side) {
      hash.invalid++;
      continue;
    }
    /* Sparse null payloads are valid zero contributions. */
    if (!item.disps) {
      continue;
    }
    for (int sample = 0; sample < item.totdisp; sample++) {
      for (int axis = 0; axis < 3; axis++) {
        hash.scalar(item.disps[sample][axis]);
      }
    }
  }
  return hash;
}

static void multires_audit_json_string(const char *text)
{
  std::putchar('"');
  for (; *text; text++) {
    const unsigned char byte = static_cast<unsigned char>(*text);
    if (byte == '"' || byte == '\\') {
      std::putchar('\\');
      std::putchar(byte);
    }
    else if (byte < 0x20) {
      std::printf("\\u%04x", unsigned(byte));
    }
    else {
      std::putchar(byte);
    }
  }
  std::putchar('"');
}

static bool sculpt_multires_base_fingerprint_poll(bContext *C)
{
  const Object *object = CTX_data_active_object(C);
  return object && object->type == OB_MESH &&
         BKE_modifiers_findby_type(object, eModifierType_Multires);
}

static wmOperatorStatus sculpt_multires_base_fingerprint_exec(bContext *C, wmOperator *op)
{
  Object *object = CTX_data_active_object(C);
  if (!object || object->type != OB_MESH) {
    return OPERATOR_CANCELLED;
  }
  const SculptSession *ss = object->runtime->sculpt_session;
  if (ss && (ss->cache || ss->filter_cache)) {
    BKE_report(op->reports, RPT_ERROR, "Release the active stroke or filter before taking a fingerprint");
    return OPERATOR_CANCELLED;
  }
  const MultiresModifierData *mmd = sculpt_multires_layer_modifier(*object);
  if (!mmd) {
    return OPERATOR_CANCELLED;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(object->data);
  const SubdivCCG *ccg = ss ? ss->subdiv_ccg : nullptr;
  char label[128];
  RNA_string_get(op->ptr, "label", label);
  const bool payloads = RNA_boolean_get(op->ptr, "include_layer_payloads");
  const bool surface = RNA_boolean_get(op->ptr, "include_surface");
  const double start = BLI_time_now_seconds();
  const MDisps *base = static_cast<const MDisps *>(
      CustomData_get_layer(&mesh.corner_data, CD_MDISPS));
  /* No grids before the first subdivision is a valid base. */
  const MultiresAuditHash native = multires_audit_disps(base, base ? mesh.corners_num : 0);
  MultiresAuditHash cage, evaluated, metadata;
  cage.word(uint32_t(mesh.verts_num));
  for (const float3 &point : mesh.vert_positions()) {
    for (int axis = 0; axis < 3; axis++) {
      cage.scalar(point[axis]);
    }
  }
  if (surface && ccg) {
    evaluated.word(uint32_t(ccg->level));
    evaluated.word(uint32_t(ccg->grids_num));
    evaluated.word(uint32_t(ccg->grid_size));
    for (const float3 &point : ccg->positions) {
      for (int axis = 0; axis < 3; axis++) {
        evaluated.scalar(point[axis]);
      }
    }
  }
  metadata.word(uint32_t(mmd->sculpt_layer_active));
  metadata.word(uint32_t(mmd->sculpt_layer_recording));
  int visible = 0;
  int index = 0;
  uint64_t layer_invalid = 0;
  std::printf("[MULTIRES_AUDIT_BEGIN] ");
  multires_audit_json_string(label);
  std::putchar('\n');
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    metadata.text(layer.name);
    metadata.word(uint32_t(layer.flag));
    metadata.scalar(layer.strength);
    metadata.word(uint32_t(layer.level));
    metadata.word(uint32_t(layer.grids_num));
    visible += (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0;
    MultiresAuditHash grids, l0;
    if (payloads) {
      /* Reject incompatible counts without walking an unknown descriptor table. */
      if (layer.grids_num < 0 ||
          (layer.grids_num != 0 && layer.grids_num != mesh.corners_num)) {
        grids.invalid++;
      }
      else {
        grids = multires_audit_disps(layer.disps, layer.grids_num);
      }
      l0.word(uint32_t(layer.base_verts_num));
      l0.word(layer.base_deltas != nullptr);
      if (layer.base_verts_num < 0 ||
          (layer.base_verts_num != 0 && layer.base_verts_num != mesh.verts_num)) {
        l0.invalid++;
      }
      else if (layer.base_deltas) {
        for (int vertex = 0; vertex < layer.base_verts_num; vertex++) {
          for (int axis = 0; axis < 3; axis++) {
            l0.scalar(layer.base_deltas[vertex][axis]);
          }
        }
      }
      else {
        l0.invalid += layer.base_verts_num != 0;
      }
    }
    layer_invalid += grids.invalid + l0.invalid;
    std::printf("[MULTIRES_LAYER_AUDIT] {\"index\":%d,\"name\":", index++);
    multires_audit_json_string(layer.name);
    std::printf(",\"level\":%d,\"visible\":%d,\"strength\":", layer.level,
                int((layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0));
    if (std::isfinite(layer.strength)) {
      std::printf("%.9g", double(layer.strength));
    }
    else {
      std::printf("null");
    }
    std::printf(
        ",\"payload_read\":%d,"
        "\"grids_hash\":\"%016llx\",\"l0_hash\":\"%016llx\",\"invalid\":%llu}\n",
        int(payloads),
        static_cast<unsigned long long>(grids.value),
        static_cast<unsigned long long>(l0.value),
        static_cast<unsigned long long>(grids.invalid + l0.invalid));
  }
  std::printf("[MULTIRES_BASE_AUDIT] {\"label\":");
  multires_audit_json_string(label);
  std::printf(
      ",\"object\":");
  multires_audit_json_string(object->id.name + 2);
  std::printf(
      ",\"modifier\":");
  multires_audit_json_string(mmd->modifier.name);
  std::printf(
      ",\"sculpt\":%d,\"top\":%d,\"verts\":%d,\"corners\":%d,\"native_available\":%d,"
      "\"active\":%d,\"recording\":%d,"
      "\"layers\":%d,\"visible\":%d,\"pending_coords\":%d,"
      "\"native_hash\":\"%016llx\",\"native_words\":%llu,"
      "\"cage_hash\":\"%016llx\",\"metadata_hash\":\"%016llx\","
      "\"surface_read\":%d,\"surface_hash\":\"%016llx\",\"invalid\":%llu,"
      "\"milliseconds\":%.3f}\n",
      mmd->sculptlvl, mmd->totlvl, mesh.verts_num, mesh.corners_num, int(base != nullptr),
      mmd->sculpt_layer_active,
      int(mmd->sculpt_layer_recording), index, visible,
      ccg ? int(ccg->dirty.coords) : 0,
      static_cast<unsigned long long>(native.value),
      static_cast<unsigned long long>(native.words),
      static_cast<unsigned long long>(cage.value),
      static_cast<unsigned long long>(metadata.value), int(surface && ccg),
      static_cast<unsigned long long>(evaluated.value),
      static_cast<unsigned long long>(native.invalid + cage.invalid + metadata.invalid +
                                      evaluated.invalid + layer_invalid),
      (BLI_time_now_seconds() - start) * 1000.0);
  std::fflush(stdout);
  BKE_report(op->reports, RPT_INFO, "Read-only Multires fingerprints written to the launch log");
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_base_fingerprint(wmOperatorType *ot)
{
  ot->name = "Multires Base Fingerprint";
  ot->idname = "SCULPT_OT_multires_base_fingerprint";
  ot->description = "Read native base, stored layers and live surface without committing or adding an Undo step";
  ot->exec = sculpt_multires_base_fingerprint_exec;
  ot->poll = sculpt_multires_base_fingerprint_poll;
  /* Intentionally no REGISTER/UNDO flags: even Repeat Last must stay untouched. */
  ot->flag = 0;
  RNA_def_string(ot->srna, "label", "manual", 128, "Label", "Label written to the launch log");
  RNA_def_boolean(ot->srna, "include_layer_payloads", true, "Read Layer Payloads", "Read every stored layer sample; explicit cost on large meshes");
  RNA_def_boolean(ot->srna, "include_surface", true, "Read Live Surface", "Read live CCG coordinates without evaluating the object");
}


/* Named color selection is a storage transaction, not a geometry rebuild. */
static bool sculpt_multires_polypaint_attribute_poll(bContext *C)
{
  const Object *object = CTX_data_active_object(C);
  return object && object->type == OB_MESH &&
         ELEM(object->mode, OB_MODE_OBJECT, OB_MODE_SCULPT) &&
         BKE_modifiers_findby_type(object, eModifierType_Multires);
}

static wmOperatorStatus sculpt_multires_polypaint_attribute_exec(bContext *C, wmOperator *op)
{
  const char *enabled = BLI_getenv("BLENDER_SCULPT_POLYPAINT_MULTI_ATTRIBUTE");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    BKE_report(op->reports, RPT_ERROR, "Enable the multi-attribute Polypaint launch profile");
    return OPERATOR_CANCELLED;
  }
  Object *object = CTX_data_active_object(C);
  Mesh &mesh = *id_cast<Mesh *>(object->data);
  SculptSession *session = object->runtime->sculpt_session;
  if (object->id.lib || mesh.id.lib || mesh.id.us != 1 ||
      !(U.uiflag & USER_GLOBALUNDO) || U.undosteps <= 0 ||
      (session && (session->cache || session->filter_cache))) {
    BKE_report(op->reports, RPT_ERROR, "Finish the stroke and use local single-user mesh data with Global Undo");
    return OPERATOR_CANCELLED;
  }
  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object->modifiers) {
    if (md.type == eModifierType_Multires) {
      if (mmd) {
        BKE_report(op->reports, RPT_ERROR, "Select an object with one Multires modifier");
        return OPERATOR_CANCELLED;
      }
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
    }
  }
  char name[68], new_name[68];
  RNA_string_get(op->ptr, "name", name);
  RNA_string_get(op->ptr, "new_name", new_name);
  const int action = RNA_enum_get(op->ptr, "action");
  const auto attributes = mesh.attributes();
  const auto meta = attributes.lookup_meta_data(name);
  if (!name[0] || (action == 1 ? attributes.contains(name) : !bke::mesh::is_color_attribute(meta)) ||
      (meta && !ELEM(meta->domain, bke::AttrDomain::Point, bke::AttrDomain::Corner)) ||
      (action == 3 && (!new_name[0] || attributes.contains(new_name)))) {
    BKE_report(op->reports, RPT_ERROR, "Choose a valid unique Color Attribute name");
    return OPERATOR_CANCELLED;
  }
  /* A removed external attribute may still own painted data. Do not reuse its
   * name silently: restore it first, or remove its master through these controls. */
  MultiresModifierData occupied{};
  if (action == 1 && (STREQ(mmd->sculpt_color_attribute, name) ||
      (!mmd->sculpt_color_attribute[0] ? false :
       bke::multires_layers::polypaint_attribute_view(*mmd, name, occupied)))) {
    BKE_report(op->reports, RPT_ERROR, "A retained Paint master already owns this name");
    return OPERATOR_CANCELLED;
  }
  UndoStack *stack = ED_undo_stack_get();
  if (!stack || stack->step_init || stack->group_level != 0 ||
      !ED_undo_is_memfile_compatible(C)) {
    BKE_report(op->reports, RPT_ERROR, "Finish the current Undo transaction first");
    return OPERATOR_CANCELLED;
  }
  const UndoStep *previous = stack->step_active;
  ED_undo_push(C, "Before Polypaint Attribute Operation");
  if (!stack->step_active || stack->step_active == previous ||
      stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE) {
    BKE_report(op->reports, RPT_ERROR, "Paint attribute checkpoint failed; no data changed");
    return OPERATOR_CANCELLED;
  }
  /* Bind old one-master files BEFORE changing the mesh's active attribute. */
  if (!mmd->sculpt_color_attribute[0] && mesh.active_color_attribute &&
      mesh.active_color_attribute[0]) {
    if (!bke::multires_layers::polypaint_attribute_activate(*mmd, mesh.active_color_attribute)) {
      return OPERATOR_CANCELLED;
    }
  }
  auto writable = mesh.attributes_for_write();
  if (action == 1) {
    if (!writable.add(name, bke::AttrDomain::Corner, bke::AttrType::ColorFloat,
                      bke::AttributeInitDefaultValue())) {
      return OPERATOR_CANCELLED;
    }
    auto colors = writable.lookup_for_write_span<ColorGeometry4f>(name);
    colors.span.fill(ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f));
    colors.finish();
  }
  if (ELEM(action, 0, 1)) {
    if (!bke::multires_layers::polypaint_attribute_activate(*mmd, name)) {
      if (action == 1) {
        writable.remove(name);
      }
      BKE_report(op->reports, RPT_ERROR, "Paint master selection failed");
      return OPERATOR_CANCELLED;
    }
    BKE_id_attributes_active_color_set(&mesh.id, name);
    if (!mesh.default_color_attribute || !mesh.default_color_attribute[0]) {
      BKE_id_attributes_default_color_set(&mesh.id, name);
    }
  }
  else if (action == 2) {
    const bool was_active = mesh.active_color_attribute && STREQ(mesh.active_color_attribute, name);
    const bool was_default = mesh.default_color_attribute && STREQ(mesh.default_color_attribute, name);
    if (!writable.remove(name)) {
      return OPERATOR_CANCELLED;
    }
    bke::multires_layers::polypaint_attribute_remove(*mmd, name);
    std::string replacement;
    writable.foreach_attribute([&](const bke::AttributeIter &attribute) {
      if (replacement.empty() && bke::mesh::is_color_attribute(
          writable.lookup_meta_data(attribute.name)) &&
          ELEM(attribute.domain, bke::AttrDomain::Point, bke::AttrDomain::Corner)) {
        replacement = std::string(attribute.name);
      }
    });
    if (was_active) {
      BKE_id_attributes_active_color_set(&mesh.id, replacement.c_str());
      if (!replacement.empty()) {
        bke::multires_layers::polypaint_attribute_activate(*mmd, replacement.c_str());
      }
    }
    if (was_default) {
      BKE_id_attributes_default_color_set(&mesh.id, replacement.c_str());
    }
  }
  else if (action == 3) {
    const bool was_active = mesh.active_color_attribute && STREQ(mesh.active_color_attribute, name);
    const bool was_default = mesh.default_color_attribute && STREQ(mesh.default_color_attribute, name);
    /* Check the archive namespace before renaming the base mesh entry. */
    MultiresModifierData collision{};
    if (mmd->sculpt_color_attribute[0] &&
        bke::multires_layers::polypaint_attribute_view(*mmd, new_name, collision)) {
      BKE_report(op->reports, RPT_ERROR, "A retained Paint master already owns the new name");
      return OPERATOR_CANCELLED;
    }
    if (!writable.rename(name, new_name)) {
      return OPERATOR_CANCELLED;
    }
    if (!bke::multires_layers::polypaint_attribute_rename(*mmd, name, new_name)) {
      writable.rename(new_name, name);
      return OPERATOR_CANCELLED;
    }
    if (was_active) BKE_id_attributes_active_color_set(&mesh.id, new_name);
    if (was_default) BKE_id_attributes_default_color_set(&mesh.id, new_name);
  }
  /* Selection only refreshes named GPU buffers. Structural attribute changes
   * also invalidate evaluated mesh attributes through the dependency graph. */
  if (session) {
    if (bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*object)) {
      IndexMaskMemory memory;
      const IndexMask nodes = bke::pbvh::all_leaf_nodes(*pbvh, memory);
      if (name[0]) pbvh->tag_attribute_changed(nodes, name);
      if (new_name[0]) pbvh->tag_attribute_changed(nodes, new_name);
    }
  }
  DEG_id_tag_update(&mesh.id, action == 0 ? ID_RECALC_SYNC_TO_EVAL : ID_RECALC_GEOMETRY);
  DEG_id_tag_update(&object->id, action == 0 ? ID_RECALC_SYNC_TO_EVAL : ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, &mesh);
  WM_main_add_notifier(NC_OBJECT | ND_MODIFIER, object);
  std::printf("[POLYPAINT_ATTRIBUTE] action=%d name=%s master=%s level=%d archived=%d\n",
              action, name, mmd->sculpt_color_attribute, mmd->sculpt_color_level,
              mmd->sculpt_color_attributes.count());
  std::fflush(stdout);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_polypaint_attribute(wmOperatorType *ot)
{
  static const EnumPropertyItem actions[] = {
      {0, "SELECT", 0, "Select", "Paint on this attribute"},
      {1, "ADD", 0, "Add", "Create a white float corner Color Attribute"},
      {2, "REMOVE", 0, "Remove", "Remove the attribute and its Paint master"},
      {3, "RENAME", 0, "Rename", "Rename the attribute and its Paint master together"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Multires Polypaint Attribute";
  ot->idname = "SCULPT_OT_multires_polypaint_attribute";
  ot->description = "Manage independent Multires Color Attributes with Undo";
  ot->exec = sculpt_multires_polypaint_attribute_exec;
  ot->poll = sculpt_multires_polypaint_attribute_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_enum(ot->srna, "action", actions, 0, "Action", "Color Attribute operation");
  RNA_def_string(ot->srna, "name", "", 68, "Attribute", "Color Attribute name");
  RNA_def_string(ot->srna, "new_name", "", 68, "New Name", "New unique name");
}

static bool sculpt_multires_merge_visible_to_base_poll(bContext *C)
{
  const Object *object = CTX_data_active_object(C);
  return object && object->type == OB_MESH && object->mode == OB_MODE_OBJECT &&
         BKE_modifiers_findby_type(object, eModifierType_Multires);
}

static wmOperatorStatus sculpt_multires_merge_visible_to_base_exec(bContext *C, wmOperator *op)
{
  const char *enabled = BLI_getenv("BLENDER_SCULPT_LAYER_BAKE_BASE");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    BKE_report(op->reports, RPT_ERROR, "Enable the experimental visible-layer bake launch profile first");
    return OPERATOR_CANCELLED;
  }
  Object *object = CTX_data_active_object(C);
  if (!object || object->type != OB_MESH || object->mode != OB_MODE_OBJECT) {
    BKE_report(op->reports, RPT_ERROR, "Switch to Object Mode before baking; pending Sculpt must commit in its original frame");
    return OPERATOR_CANCELLED;
  }
  /* MERGE_VISIBLE_PRECISE_ERRORS_V01: these guards protect independent states.
   * Report the exact failing precondition instead of saying "local object" for
   * unrelated cases such as stale SculptSession or disabled Global Undo.
   * Do not relax safety checks or discard pending CCG data during diagnosis. */
  if (object->id.lib) {
    BKE_report(op->reports, RPT_ERROR,
               "Bake blocked: Object is linked from an external library; make the Object local");
    return OPERATOR_CANCELLED;
  }
  if (object->adt) {
    BKE_report(op->reports, RPT_ERROR,
               "Bake blocked: Object has animation data or drivers; use a safe unanimated copy");
    return OPERATOR_CANCELLED;
  }
  if (object->runtime->sculpt_session) {
    BKE_report(op->reports, RPT_ERROR,
               "Bake blocked: SculptSession still exists in Object Mode; verify sculpt exit/flush");
    return OPERATOR_CANCELLED;
  }
  if (!(U.uiflag & USER_GLOBALUNDO)) {
    BKE_report(op->reports, RPT_ERROR,
               "Bake blocked: enable Global Undo in Preferences > System");
    return OPERATOR_CANCELLED;
  }
  if (U.undosteps <= 0) {
    BKE_report(op->reports, RPT_ERROR,
               "Bake blocked: set Undo Steps greater than zero in Preferences");
    return OPERATOR_CANCELLED;
  }
  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object->modifiers) {
    if (md.type == eModifierType_Multires) {
      if (mmd) {
        BKE_report(op->reports, RPT_ERROR, "Bake requires one Multires modifier");
        return OPERATOR_CANCELLED;
      }
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
    }
  }
  if (!mmd || mmd->sculpt_displacement_enabled || mmd->sculpt_shrinkwrap_enabled) {
    BKE_report(op->reports, RPT_ERROR, "Disable live displacement and shrinkwrap before baking visible layers");
    return OPERATOR_CANCELLED;
  }
  const bool remove_layers = RNA_boolean_get(op->ptr, "remove_layers");
  Mesh &mesh = *id_cast<Mesh *>(object->data);
  if (const char *error = bke::multires_layers::merge_visible_to_base(
          *mmd, mesh, remove_layers, true))
  {
    BKE_report(op->reports, RPT_ERROR, error);
    return OPERATOR_CANCELLED;
  }
  UndoStack *stack = ED_undo_stack_get();
  if (stack && (stack->step_init || stack->group_level != 0)) {
    BKE_report(op->reports, RPT_ERROR, "Finish the active Undo transaction before baking");
    return OPERATOR_CANCELLED;
  }
  const UndoStep *previous = stack ? stack->step_active : nullptr;
  /* The last visible scene state may be a Sculpt Position step. Capture the
   * already committed Object Mode base AND layer list before removing entries.
   * The operator's normal global Undo push captures the post-bake state. */
  ED_undo_push(C, "Before Bake Visible Sculpt Layers");
  stack = ED_undo_stack_get();
  if (!stack || stack->step_init || !stack->step_active || stack->step_active == previous ||
      stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE)
  {
    BKE_report(op->reports, RPT_ERROR, "Pre-bake Undo checkpoint failed; no layer baked");
    return OPERATOR_CANCELLED;
  }
  if (const char *error = bke::multires_layers::merge_visible_to_base(*mmd, mesh, remove_layers)) {
    BKE_report(op->reports, RPT_ERROR, error);
    return OPERATOR_CANCELLED;
  }
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_OBJECT | ND_MODIFIER, object);
  std::printf("[LAYER_BAKE_BASE] visible_only=1 remove=%d sculpt=%d top=%d retained_layers=%d\n",
              int(remove_layers), mmd->sculptlvl, mmd->totlvl, mmd->sculpt_layers.count());
  std::fflush(stdout);
  BKE_report(op->reports, RPT_INFO, "Visible layers baked into native Multires; hidden layers retained");
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_merge_visible_to_base(wmOperatorType *ot)
{
  ot->name = "Bake Visible Sculpt Layers into Base";
  ot->idname = "SCULPT_OT_multires_merge_visible_to_base";
  ot->description = "Bake visible contributions at full resolution into native Multires and remove or empty their layers";
  ot->poll = sculpt_multires_merge_visible_to_base_poll;
  ot->exec = sculpt_multires_merge_visible_to_base_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_boolean(ot->srna, "remove_layers", true, "Remove Baked Layers",
                  "Remove baked visible layers; otherwise retain empty layer slots with zero contribution");
}

static bool sculpt_multires_layer_commit(Object &ob,
                                        wmOperator &op,
                                        bool *r_local_surface_preserved = nullptr)
{
  const bool trace = []() {
    const char *value = BLI_getenv("BLENDER_LAYER_RESP_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();

  const double total_start = trace ? BLI_time_now_seconds() : 0.0;
  const bool ok = bke::multires_layers::commit(ob, r_local_surface_preserved);
  const double total_ms = trace ? (BLI_time_now_seconds() - total_start) * 1000.0 : 0.0;

  if (trace) {
    std::printf("[LAYER_COMMIT_OP] ok=%d total=%.3fms\n", ok ? 1 : 0, total_ms);
  }

  if (!ok) {
    BKE_report(op.reports,
               RPT_ERROR,
               "Layer was not recorded; pending sculpt data retained. Restore the sculpt "
               "level used for the strokes, visible active layer and non-zero strength before retrying");
    return false;
  }
  return true;
}

static wmOperatorStatus sculpt_multires_layer_commit_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (!mmd || !bke::multires_layers::active(*mmd)) return OPERATOR_CANCELLED;
  bool local_surface_preserved = false;
  if (!sculpt_multires_layer_commit(*ob, *op, &local_surface_preserved)) {
    return OPERATOR_CANCELLED;
  }

  /* COMMIT_KEEP_CCG_V1: explicit Commit can be metadata-only after a certified
   * local layer publication. Keep the current CCG/PBVH and synchronize the
   * persistent modifier data, as empty Add Layer already does. This does not
   * cancel geometry updates that other operations have already requested. */
  const char *keep_env = BLI_getenv("BLENDER_SCULPT_COMMIT_KEEP_CCG");
  const bool opt_in = keep_env && keep_env[0] == '1' && keep_env[1] == '\0';
  const SculptSession *ss = ob->runtime->sculpt_session;
  bool keep_ccg = opt_in && local_surface_preserved && ss && ss->subdiv_ccg &&
                  ss->multires_modifier == mmd &&
                  !mmd->sculpt_displacement_enabled && !mmd->sculpt_shrinkwrap_enabled;
  /* Later enabled modifiers may require their own geometry evaluation. */
  if (keep_ccg) {
    for (ModifierData *md = mmd->modifier.next; md; md = md->next) {
      if (BKE_modifier_is_enabled(CTX_data_scene(C), md, eModifierMode_Realtime)) {
        keep_ccg = false;
        break;
      }
    }
  }
  const char *trace_env = BLI_getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double request_start = trace ? BLI_time_now_seconds() : 0.0;
  DEG_id_tag_update(&ob->id, keep_ccg ? ID_RECALC_SYNC_TO_EVAL : ID_RECALC_GEOMETRY);
  if (trace) {
    std::printf("[LAYER_COMMIT_REFRESH] request=%s opt_in=%d local_surface_preserved=%d "
                "tag=%.3fms\n",
                keep_ccg ? "SYNC_ONLY" : "GEOMETRY",
                int(opt_in),
                int(local_surface_preserved),
                (BLI_time_now_seconds() - request_start) * 1000.0);
    std::fflush(stdout);
  }
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  BKE_report(op->reports,
             RPT_INFO,
             bke::multires_layers::base_has_visible_layers(*mmd) ?
                 "Layer recorded in memory. Save the .blend to keep it on disk" :
                 "Native Multires base recorded. Hidden layer deltas preserved");
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_commit(wmOperatorType *ot)
{
  ot->name = "Commit Pending Layer Changes";
  ot->idname = "SCULPT_OT_multires_layer_commit";
  ot->description = "Record pending strokes in the active layer without merging; save the blend file separately";
  ot->exec = sculpt_multires_layer_commit_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_layer_snapshot_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (!mmd) return OPERATOR_CANCELLED;
  MultiresSculptLayer *source = bke::multires_layers::active(*mmd);
  if (!source || !sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  const int original_index = mmd->sculpt_layer_active;
  MultiresSculptLayer *snapshot = bke::multires_layers::duplicate(*mmd, *source);
  BLI_snprintf(snapshot->name, sizeof(snapshot->name), "%.48s - copy", source->name);
  bke::multires_layers::set_visible(*snapshot, false);
  bke::multires_layers::set_active(*mmd, original_index);
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  BKE_report(op->reports, RPT_INFO, "Hidden additive copy created (not a restore point). Save the .blend to keep it on disk");
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_snapshot(wmOperatorType *ot)
{
  ot->name = "Create Hidden Layer Copy";
  ot->idname = "SCULPT_OT_multires_layer_snapshot";
  ot->description = "Record strokes and create a hidden copy of the active layer without changing the visible shape or recording target";
  ot->exec = sculpt_multires_layer_snapshot_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_base_capture_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  Mesh *mesh = BKE_mesh_from_object(ob);
  if (!mmd || !mesh || mmd->sculpt_layers.is_empty()) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  if (!bke::multires_layers::bake_visible_to_base(*mmd, *mesh)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot bake: remove hidden copies, use single-user mesh data and valid finite grids");
    return OPERATOR_CANCELLED;
  }
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_base_capture(wmOperatorType *ot)
{
  ot->name = "Bake Visible Layers into Base";
  ot->idname = "SCULPT_OT_multires_base_capture";
  ot->description = "Merge contributions into the base and clear layer offsets. Hidden layers and shared mesh data are rejected. Save a copy first";
  ot->exec = sculpt_multires_base_capture_exec;
  ot->invoke = WM_operator_confirm;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_texture_displacement_bake_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return OPERATOR_CANCELLED;
  }
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (!mmd || !bke::multires_layers::active(*mmd)) {
    BKE_report(op->reports, RPT_ERROR, "An active Multires Sculpt Layer is required");
    return OPERATOR_CANCELLED;
  }
  if (!sculpt_multires_layer_commit(*ob, *op)) {
    return OPERATOR_CANCELLED;
  }

  const bool respect_mask = RNA_boolean_get(op->ptr, "respect_mask");
  const bool disable_live = RNA_boolean_get(op->ptr, "disable_live");
  bool used_mask = false;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  if (!bke::multires_layers::bake_texture_displacement_to_active(
          depsgraph, *ob, respect_mask, used_mask))
  {
    BKE_report(op->reports, RPT_ERROR,
               "Texture displacement could not be baked into the active Sculpt Layer");
    return OPERATOR_CANCELLED;
  }

  if (disable_live) {
    mmd->sculpt_displacement_enabled = 0;
  }

  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  BKE_report(op->reports, RPT_INFO, "Texture displacement baked into the active Sculpt Layer");
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_texture_displacement_bake(wmOperatorType *ot)
{
  ot->name = "Bake Texture Displacement to Active Sculpt Layer";
  ot->idname = "SCULPT_OT_multires_texture_displacement_bake";
  ot->description =
      "Convert the procedural Multires texture displacement into tangent-space deltas in the "
      "active Sculpt Layer";
  ot->exec = sculpt_multires_texture_displacement_bake_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "respect_mask",
                  true,
                  "Respect Sculpt Mask",
                  "Bake only the unmasked fraction of the texture displacement");
  RNA_def_boolean(ot->srna,
                  "disable_live",
                  true,
                  "Disable Live Displacement",
                  "Disable procedural displacement after baking; masked portions are excluded "
                  "from the layer");
}

static wmOperatorStatus sculpt_multires_shrinkwrap_bake_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return OPERATOR_CANCELLED;
  }
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (!mmd || !bke::multires_layers::active(*mmd)) {
    BKE_report(op->reports, RPT_ERROR, "An active Multires Sculpt Layer is required");
    return OPERATOR_CANCELLED;
  }
  if (!sculpt_multires_layer_commit(*ob, *op)) {
    return OPERATOR_CANCELLED;
  }
  const bool respect_mask = RNA_boolean_get(op->ptr, "respect_mask");
  bool used_mask = false;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  if (!bke::multires_layers::bake_shrinkwrap_to_active(depsgraph, *ob, respect_mask, used_mask)) {
    BKE_report(op->reports, RPT_ERROR, "Shrinkwrap could not be baked to the active Sculpt Layer");
    return OPERATOR_CANCELLED;
  }
  /* The baked layer already contains the chosen masked contribution. Keeping the live
   * projection active would apply it again to the unmasked region. */
  mmd->sculpt_shrinkwrap_enabled = 0;
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_shrinkwrap_bake(wmOperatorType *ot)
{
  ot->name = "Bake Shrinkwrap to Active Sculpt Layer";
  ot->idname = "SCULPT_OT_multires_shrinkwrap_bake";
  ot->description = "Store the configured Multires Shrinkwrap projection in the active layer, including at level zero";
  ot->exec = sculpt_multires_shrinkwrap_bake_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_boolean(ot->srna,
                  "respect_mask",
                  true,
                  "Respect Sculpt Mask",
                  "Bake only the unmasked contribution and disable live projection");
}

static wmOperatorStatus sculpt_multires_layer_add_exec(bContext *C, wmOperator *op)
{
  const bool trace = []() {
    const char *value = BLI_getenv("BLENDER_LAYER_RESP_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  const double total_start = trace ? BLI_time_now_seconds() : 0.0;

  double commit_ms = 0.0;
  double mesh_ms = 0.0;
  double compatible_ms = 0.0;
  double add_ms = 0.0;
  double vertex_ids_ms = 0.0;
  double l0_ms = 0.0;
  double corner_ids_ms = 0.0;
  double rebuild_ms = 0.0;
  double depsgraph_ms = 0.0;

  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) {
    return OPERATOR_CANCELLED;
  }

  double stage_start = trace ? BLI_time_now_seconds() : 0.0;
  const bool commit_ok = sculpt_multires_layer_commit(*ob, *op);
  if (trace) {
    commit_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
  }
  if (!commit_ok) {
    return OPERATOR_CANCELLED;
  }

  char name[64];
  BLI_snprintf(name, sizeof(name), "Layer %d", mmd->sculpt_layers.count() + 1);

  stage_start = trace ? BLI_time_now_seconds() : 0.0;
  Mesh *mesh = BKE_mesh_from_object(ob);
  if (trace) {
    mesh_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
  }

  bool compatible = false;
  if (mesh) {
    stage_start = trace ? BLI_time_now_seconds() : 0.0;
    compatible = bke::multires_layers::compatible(*mmd, *mesh);
    if (trace) {
      compatible_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
    }
  }

  bool add_ok = false;
  if (mesh && compatible) {
    stage_start = trace ? BLI_time_now_seconds() : 0.0;
    add_ok = bke::multires_layers::add(*mmd, name, mmd->totlvl, mesh->corners_num);
    if (trace) {
      add_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
    }
  }

  if (!mesh || !compatible || !add_ok) {
    BKE_report(op->reports, RPT_ERROR, "Cannot add layer: incompatible Multires grids");
    return OPERATOR_CANCELLED;
  }

  stage_start = trace ? BLI_time_now_seconds() : 0.0;
  bke::multires_layers::base_ensure_vertex_ids(*mesh);
  if (trace) {
    vertex_ids_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
  }

  if (mmd->sculptlvl == 0) {
    stage_start = trace ? BLI_time_now_seconds() : 0.0;
    bke::multires_layers::base_deltas_ensure(
        *bke::multires_layers::active(*mmd), mesh->verts_num);
    mmd->sculpt_layer_recording = 1;
    if (trace) {
      l0_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
    }
  }

  if (mmd->totlvl > 0) {
    stage_start = trace ? BLI_time_now_seconds() : 0.0;
    bke::multires_layers::ensure_corner_ids(*mmd, *mesh);
    if (trace) {
      corner_ids_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
    }
  }

  /* LAYER_ADD_METADATA_FAST_V2
   * Adding a brand-new empty additive layer is geometry-neutral. The live CCG
   * already represents the correct visible surface, and destroying the PBVH
   * here only forces an expensive lazy rebuild whose cost scales with Multires
   * resolution. Keep the SculptSession/PBVH alive and only synchronize the
   * modifier metadata to the evaluated object.
   *
   * Set BLENDER_LAYER_ADD_FORCE_REBUILD=1 to restore the old behavior for A/B
   * testing or as a conservative fallback while validating this path. */
  bool force_rebuild = false;
  if (const char *value = BLI_getenv("BLENDER_LAYER_ADD_FORCE_REBUILD")) {
    force_rebuild = value[0] != '\0' && value[0] != '0';
  }

  if (force_rebuild) {
    stage_start = trace ? BLI_time_now_seconds() : 0.0;
    multires_force_sculpt_rebuild(ob);
    if (trace) {
      rebuild_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
    }
  }

  stage_start = trace ? BLI_time_now_seconds() : 0.0;
  DEG_id_tag_update(
      &ob->id, force_rebuild ? ID_RECALC_GEOMETRY : ID_RECALC_SYNC_TO_EVAL);
  if (trace) {
    depsgraph_ms = (BLI_time_now_seconds() - stage_start) * 1000.0;
  }

  /* Layer-list/active-layer UI changed even though geometry did not. */
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);

  if (trace) {
    const double total_ms = (BLI_time_now_seconds() - total_start) * 1000.0;
    std::printf(
        "[LAYER_ADD] mode=%s level=%d total_level=%d verts=%d corners=%d layers=%d "
        "commit=%.3fms mesh=%.3fms compatible=%.3fms add=%.3fms "
        "vertex_ids=%.3fms l0=%.3fms corner_ids=%.3fms rebuild=%.3fms "
        "depsgraph=%.3fms total=%.3fms\n",
        force_rebuild ? "FULL_REBUILD" : "METADATA_ONLY",
        int(mmd->sculptlvl),
        int(mmd->totlvl),
        mesh->verts_num,
        mesh->corners_num,
        mmd->sculpt_layers.count(),
        commit_ms,
        mesh_ms,
        compatible_ms,
        add_ms,
        vertex_ids_ms,
        l0_ms,
        corner_ids_ms,
        rebuild_ms,
        depsgraph_ms,
        total_ms);
  }

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_add(wmOperatorType *ot)
{
  ot->name = "Add Sculpt Layer";
  ot->idname = "SCULPT_OT_multires_layer_add";
  ot->description = "Add an empty persistent sculpt layer to the Multires modifier";
  ot->exec = sculpt_multires_layer_add_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}


/* LAYER_DELETE_FAST_V1
 *
 * Delete has three paths:
 *  - metadata-only when the layer has no visible contribution;
 *  - partial CCG re-evaluation when only sparse high-resolution grids contribute;
 *  - full rebuild when the coarse cage (L0), shrinkwrap, or runtime state makes
 *    an in-place update unsafe.
 */

static bool sculpt_multires_layer_has_base_contribution(const MultiresSculptLayer &layer)
{
  if (layer.base_deltas == nullptr || layer.base_verts_num <= 0) {
    return false;
  }
  for (int i = 0; i < layer.base_verts_num; i++) {
    const float *d = layer.base_deltas[i];
    if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
      return true;
    }
  }
  return false;
}

static BitVector<> sculpt_multires_layer_dirty_faces(const Mesh &mesh,
                                                     const MultiresSculptLayer &layer)
{
  BitVector<> dirty(mesh.faces_num, false);
  if (layer.disps == nullptr || layer.grids_num != mesh.corners_num) {
    return dirty;
  }

  const OffsetIndices faces = mesh.faces();
  for (const int face : faces.index_range()) {
    for (const int corner : faces[face]) {
      if (layer.disps[corner].disps != nullptr) {
        dirty[face].set();
        break;
      }
    }
  }

  /* Boundary stitching and normal averaging can touch a direct neighbor.
   * Expand one vertex-adjacent ring so the partial refresh and PBVH tagging
   * conservatively cover those seams as well. */
  const GroupedSpan<int> vert_to_face = mesh.vert_to_face_map();
  const Span<int> corner_verts = mesh.corner_verts();
  BitVector<> expanded(mesh.faces_num, false);
  for (const int face : faces.index_range()) {
    if (!dirty[face]) {
      continue;
    }
    expanded[face].set();
    for (const int corner : faces[face]) {
      const int vert = corner_verts[corner];
      for (const int neighbor : vert_to_face[vert]) {
        expanded[neighbor].set();
      }
    }
  }
  return expanded;
}

static int sculpt_multires_count_bits(const BitVector<> &bits)
{
  int count = 0;
  for (const int i : bits.index_range()) {
    if (bits[i]) {
      count++;
    }
  }
  return count;
}

static bool sculpt_multires_layer_delete_refresh_partial(Object &ob,
                                                         MultiresModifierData &mmd,
                                                         Mesh &mesh,
                                                         const BitVector<> &dirty_faces)
{
  if (ob.runtime == nullptr || ob.runtime->sculpt_session == nullptr ||
      dirty_faces.is_empty())
  {
    return false;
  }

  SculptSession &ss = *ob.runtime->sculpt_session;
  SubdivCCG *ccg = ss.subdiv_ccg;
  if (ccg == nullptr || ccg->subdiv == nullptr || ss.pbvh == nullptr ||
      ccg->grids_num != mesh.corners_num || ccg->level != mmd.sculptlvl)
  {
    return false;
  }

  bke::subdiv::Subdiv *subdiv = ccg->subdiv;
  bke::subdiv::displacement_attach_from_multires(
      subdiv, &mesh, &mmd, multires_texture_mapping_matrix(mmd, ob));

  IndexMaskMemory face_memory;
  const IndexMask face_mask = IndexMask::from_predicate(
      IndexRange(mesh.faces_num),
      face_memory,
      [&](const int face) { return dirty_faces[face]; },
      exec_mode::grain_size(64));

  if (!BKE_subdiv_ccg_evaluate_faces_existing(*ccg, *subdiv, face_mask, nullptr)) {
    return false;
  }

  BitVector<> dirty_grids(ccg->grids_num, false);
  const OffsetIndices faces = mesh.faces();
  for (const int face : faces.index_range()) {
    if (!dirty_faces[face]) {
      continue;
    }
    for (const int grid : faces[face]) {
      if (grid >= 0 && grid < ccg->grids_num) {
        dirty_grids[grid].set();
      }
    }
  }

  bke::pbvh::Tree &pbvh = *ss.pbvh;
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  IndexMaskMemory node_memory;
  const IndexMask changed_nodes = IndexMask::from_predicate(
      nodes.index_range(),
      node_memory,
      [&](const int node_i) {
        for (const int grid : nodes[node_i].grids()) {
          if (grid >= 0 && grid < dirty_grids.size() && dirty_grids[grid]) {
            return true;
          }
        }
        return false;
      },
      exec_mode::grain_size(1));

  if (changed_nodes.is_empty()) {
    return false;
  }

  pbvh.tag_positions_changed(changed_nodes);
  pbvh.update_bounds_grids(ccg->positions, ccg->grid_area);
  pbvh.flush_bounds_to_parents();
  pbvh.clear_changes();
  ccg->dirty.coords = false;
  return true;
}


static wmOperatorStatus sculpt_multires_layer_delete_exec(bContext *C, wmOperator *op)
{
  const bool trace = []() {
    const char *value = BLI_getenv("BLENDER_LAYER_RESP_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  const double total_start = trace ? BLI_time_now_seconds() : 0.0;

  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) {
    return OPERATOR_CANCELLED;
  }
  MultiresSculptLayer *layer = bke::multires_layers::active(*mmd);
  if (layer == nullptr) {
    return OPERATOR_CANCELLED;
  }

  double t = trace ? BLI_time_now_seconds() : 0.0;
  if (!sculpt_multires_layer_commit(*ob, *op)) {
    return OPERATOR_CANCELLED;
  }
  const double commit_ms = trace ? (BLI_time_now_seconds() - t) * 1000.0 : 0.0;

  Mesh *mesh = BKE_mesh_from_object(ob);
  if (mesh == nullptr) {
    return OPERATOR_CANCELLED;
  }

  t = trace ? BLI_time_now_seconds() : 0.0;

  const bool effective = bke::multires_layers::is_effective(*layer);
  const bool has_base = effective && sculpt_multires_layer_has_base_contribution(*layer);
  BitVector<> dirty_faces = effective ? sculpt_multires_layer_dirty_faces(*mesh, *layer) :
                                       BitVector<>(mesh->faces_num, false);
  const int dirty_face_count = sculpt_multires_count_bits(dirty_faces);
  const bool has_highres = effective && dirty_face_count > 0;

  bool force_rebuild = false;
  if (const char *value = BLI_getenv("BLENDER_LAYER_DELETE_FORCE_REBUILD")) {
    force_rebuild = value[0] != '\0' && value[0] != '0';
  }

  enum class DeleteMode {
    MetadataOnly,
    PartialCCG,
    FullRebuild,
  };

  DeleteMode mode = DeleteMode::MetadataOnly;
  if (force_rebuild || has_base || mmd->sculpt_shrinkwrap_enabled) {
    mode = DeleteMode::FullRebuild;
  }
  else if (has_highres) {
    mode = DeleteMode::PartialCCG;
  }

  const double classify_ms = trace ? (BLI_time_now_seconds() - t) * 1000.0 : 0.0;

  t = trace ? BLI_time_now_seconds() : 0.0;
  bke::multires_layers::remove(*mmd, *layer);
  const double remove_ms = trace ? (BLI_time_now_seconds() - t) * 1000.0 : 0.0;

  double refresh_ms = 0.0;
  bool partial_ok = false;

  if (mode == DeleteMode::PartialCCG) {
    t = trace ? BLI_time_now_seconds() : 0.0;
    partial_ok = sculpt_multires_layer_delete_refresh_partial(*ob, *mmd, *mesh, dirty_faces);
    refresh_ms = trace ? (BLI_time_now_seconds() - t) * 1000.0 : 0.0;
    if (!partial_ok) {
      mode = DeleteMode::FullRebuild;
    }
  }

  if (mode == DeleteMode::FullRebuild) {
    t = trace ? BLI_time_now_seconds() : 0.0;

    /* Never flush the old CCG after remove(): it can still contain the deleted
     * contribution. Discard PBVH/CCG directly and let depsgraph rebuild from
     * the true base plus the remaining layer stack. */
    if (ob->runtime->sculpt_session) {
      BKE_sculptsession_free_pbvh(*ob);
    }

    refresh_ms += trace ? (BLI_time_now_seconds() - t) * 1000.0 : 0.0;
  }

  t = trace ? BLI_time_now_seconds() : 0.0;
  DEG_id_tag_update(&ob->id,
                    mode == DeleteMode::FullRebuild ? ID_RECALC_GEOMETRY :
                                                      ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  const double depsgraph_ms = trace ? (BLI_time_now_seconds() - t) * 1000.0 : 0.0;

  if (trace) {
    const char *mode_name = mode == DeleteMode::MetadataOnly ? "METADATA_ONLY" :
                            mode == DeleteMode::PartialCCG ? "PARTIAL_CCG" :
                                                           "FULL_REBUILD";
    const double total_ms = (BLI_time_now_seconds() - total_start) * 1000.0;
    std::printf(
        "[LAYER_DELETE] mode=%s effective=%d base=%d highres=%d faces=%d/%d "
        "commit=%.3fms classify=%.3fms remove=%.3fms refresh=%.3fms "
        "depsgraph=%.3fms total=%.3fms\n",
        mode_name,
        effective ? 1 : 0,
        has_base ? 1 : 0,
        has_highres ? 1 : 0,
        dirty_face_count,
        mesh->faces_num,
        commit_ms,
        classify_ms,
        remove_ms,
        refresh_ms,
        depsgraph_ms,
        total_ms);
  }

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_delete(wmOperatorType *ot)
{
  ot->name = "Delete Sculpt Layer";
  ot->idname = "SCULPT_OT_multires_layer_delete";
  ot->description = "Delete the active persistent sculpt layer";
  ot->exec = sculpt_multires_layer_delete_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_layer_duplicate_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  MultiresSculptLayer *layer = bke::multires_layers::active(*mmd);
  if (layer == nullptr) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  bke::multires_layers::duplicate(*mmd, *layer);
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_duplicate(wmOperatorType *ot)
{
  ot->name = "Duplicate Sculpt Layer";
  ot->idname = "SCULPT_OT_multires_layer_duplicate";
  ot->description = "Duplicate the active persistent sculpt layer";
  ot->exec = sculpt_multires_layer_duplicate_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_layer_set_active_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  const int index = RNA_int_get(op->ptr, "index");
  if (index >= mmd->sculpt_layers.count()) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  bke::multires_layers::set_active(*mmd, index);
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_set_active(wmOperatorType *ot)
{
  ot->name = "Set Active Sculpt Layer";
  ot->idname = "SCULPT_OT_multires_layer_set_active";
  ot->description = "Make the selected sculpt layer active";
  ot->exec = sculpt_multires_layer_set_active_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Layer", "Layer index", 0, 1000);
}


static wmOperatorStatus sculpt_multires_layer_move_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }

  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr || mmd->sculpt_layers.count() < 2) {
    return OPERATOR_CANCELLED;
  }

  const int direction = RNA_enum_get(op->ptr, "direction");
  const int from_index = mmd->sculpt_layer_active;
  const int to_index = std::clamp(from_index + direction, 0, mmd->sculpt_layers.count() - 1);
  if (to_index == from_index) {
    return OPERATOR_CANCELLED;
  }

  /* Commit pending edits against the old order before moving the list item. */
  if (!sculpt_multires_layer_commit(*ob, *op) ||
      !bke::multires_layers::move(*mmd, from_index, to_index))
  {
    return OPERATOR_CANCELLED;
  }

  /* Current stack composition is additive, so reorder is geometry-neutral.
   * Avoid an unnecessary PBVH/SubdivCCG rebuild. The new order immediately
   * controls UI ordering and the neighbour consumed by Merge Down. */
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_move(wmOperatorType *ot)
{
  static const EnumPropertyItem direction_items[] = {
      {-1, "UP", ICON_TRIA_UP, "Up", "Move the active sculpt layer up in the stack"},
      {1, "DOWN", ICON_TRIA_DOWN, "Down", "Move the active sculpt layer down in the stack"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Move Sculpt Layer";
  ot->idname = "SCULPT_OT_multires_layer_move";
  ot->description = "Reorder the active Multires sculpt layer";
  ot->exec = sculpt_multires_layer_move_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_enum(ot->srna, "direction", direction_items, -1, "Direction", "Move direction");
}



static wmOperatorStatus sculpt_multires_layer_group_create_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;

  if (!bke::multires_layers::group_create_from_active(*mmd)) {
    BKE_report(op->reports, RPT_ERROR, "Select a Sculpt Layer first");
    return OPERATOR_CANCELLED;
  }

  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_group_create(wmOperatorType *ot)
{
  ot->name = "New Sculpt Layer Group";
  ot->idname = "SCULPT_OT_multires_layer_group_create";
  ot->description = "Create a new organizational folder containing the active Sculpt Layer";
  ot->exec = sculpt_multires_layer_group_create_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_layer_group_add_active_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;

  const int group_id = RNA_int_get(op->ptr, "group_id");
  if (!bke::multires_layers::group_add_active(*mmd, group_id)) {
    BKE_report(op->reports, RPT_ERROR, "Could not add active layer to this group");
    return OPERATOR_CANCELLED;
  }

  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_group_add_active(wmOperatorType *ot)
{
  ot->name = "Add Active Layer to Group";
  ot->idname = "SCULPT_OT_multires_layer_group_add_active";
  ot->description = "Move the active Sculpt Layer into this folder";
  ot->exec = sculpt_multires_layer_group_add_active_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "group_id", 0, 0, INT_MAX, "Group", "Group identifier", 0, INT_MAX);
}

static wmOperatorStatus sculpt_multires_layer_group_merge_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;

  const int group_id = RNA_int_get(op->ptr, "group_id");
  if (!bke::multires_layers::group_merge(*mmd, group_id)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Merge Group requires at least two contiguous visible compatible layers");
    return OPERATOR_CANCELLED;
  }

  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_group_merge(wmOperatorType *ot)
{
  ot->name = "Merge Sculpt Layer Group";
  ot->idname = "SCULPT_OT_multires_layer_group_merge";
  ot->description = "Merge all visible compatible layers in this folder into one Sculpt Layer";
  ot->exec = sculpt_multires_layer_group_merge_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "group_id", 0, 0, INT_MAX, "Group", "Group identifier", 0, INT_MAX);
}

static wmOperatorStatus sculpt_multires_layer_group_with_below_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  if (!bke::multires_layers::group_with_below(*mmd)) {
    BKE_report(op->reports, RPT_ERROR, "Select a layer with another layer below it");
    return OPERATOR_CANCELLED;
  }
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_group_with_below(wmOperatorType *ot)
{
  ot->name = "Group Sculpt Layer With Below";
  ot->idname = "SCULPT_OT_multires_layer_group_with_below";
  ot->description = "Create or extend an organizational folder using the active layer and the layer below";
  ot->exec = sculpt_multires_layer_group_with_below_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_layer_group_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  if (!bke::multires_layers::group_remove_active(*mmd)) {
    BKE_report(op->reports, RPT_ERROR, "Active layer is not in a group");
    return OPERATOR_CANCELLED;
  }
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_group_remove(wmOperatorType *ot)
{
  ot->name = "Remove Sculpt Layer From Group";
  ot->idname = "SCULPT_OT_multires_layer_group_remove";
  ot->description = "Remove the active Sculpt Layer from its organizational folder";
  ot->exec = sculpt_multires_layer_group_remove_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_layer_group_visibility_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  const int group_id = RNA_int_get(op->ptr, "group_id");
  if (!bke::multires_layers::group_toggle_visibility(*mmd, group_id)) {
    return OPERATOR_CANCELLED;
  }
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_group_visibility(wmOperatorType *ot)
{
  ot->name = "Toggle Sculpt Layer Group Visibility";
  ot->idname = "SCULPT_OT_multires_layer_group_visibility";
  ot->description = "Hide or show all layers in a Sculpt Layer folder while preserving individual visibility";
  ot->exec = sculpt_multires_layer_group_visibility_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "group_id", 0, 0, INT_MAX, "Group", "Group identifier", 0, INT_MAX);
}

static wmOperatorStatus sculpt_multires_layer_group_collapse_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  const int group_id = RNA_int_get(op->ptr, "group_id");
  if (!bke::multires_layers::group_toggle_collapsed(*mmd, group_id)) {
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_group_collapse(wmOperatorType *ot)
{
  ot->name = "Collapse Sculpt Layer Group";
  ot->idname = "SCULPT_OT_multires_layer_group_collapse";
  ot->description = "Collapse or expand an organizational Sculpt Layer folder";
  ot->exec = sculpt_multires_layer_group_collapse_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "group_id", 0, 0, INT_MAX, "Group", "Group identifier", 0, INT_MAX);
}

static wmOperatorStatus sculpt_multires_layer_merge_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  if (!bke::multires_layers::merge_down(*mmd)) {
    BKE_report(op->reports, RPT_ERROR, "Merge requires two visible layers with matching grids");
    return OPERATOR_CANCELLED;
  }
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_merge(wmOperatorType *ot)
{
  ot->name = "Merge Sculpt Layer Down";
  ot->idname = "SCULPT_OT_multires_layer_merge";
  ot->description = "Merge the active sculpt layer into the layer below";
  ot->exec = sculpt_multires_layer_merge_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_layer_flatten_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr || mmd->sculpt_layers.is_empty()) return OPERATOR_CANCELLED;
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (!(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE)) {
      BKE_report(op->reports, RPT_ERROR, "Flatten would discard hidden layers or snapshots; operation cancelled");
      return OPERATOR_CANCELLED;
    }
  }
  if (!bke::multires_layers::compatible(*mmd, *BKE_mesh_from_object(ob))) {
    BKE_report(op->reports, RPT_ERROR, "Cannot flatten incompatible layer grids");
    return OPERATOR_CANCELLED;
  }
  bke::multires_layers::flatten(*mmd);
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_flatten(wmOperatorType *ot)
{
  ot->name = "Flatten Sculpt Layers";
  ot->idname = "SCULPT_OT_multires_layer_flatten";
  ot->description = "Flatten all sculpt layers into one layer";
  ot->exec = sculpt_multires_layer_flatten_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus sculpt_multires_layer_solo_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) return OPERATOR_CANCELLED;
  MultiresModifierData *mmd = sculpt_multires_layer_modifier(*ob);
  if (mmd == nullptr || mmd->sculpt_layers.is_empty()) return OPERATOR_CANCELLED;
  const int index = RNA_int_get(op->ptr, "index");
  if (!sculpt_multires_layer_commit(*ob, *op)) return OPERATOR_CANCELLED;
  bke::multires_layers::set_solo(*mmd, index);
  multires_force_sculpt_rebuild(ob);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  return OPERATOR_FINISHED;
}

static void SCULPT_OT_multires_layer_solo(wmOperatorType *ot)
{
  ot->name = "Solo Sculpt Layer";
  ot->idname = "SCULPT_OT_multires_layer_solo";
  ot->description = "Show only the selected sculpt layer";
  ot->exec = sculpt_multires_layer_solo_exec;
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Layer", "Layer index to solo", 0, 1000);
}

/* -------------------------------------------------------------------- */
/** \name Set Persistent Base Operator
 * \{ */

static wmOperatorStatus set_persistent_base_exec(bContext *C, wmOperator * /*op*/)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  SculptSession *ss = ob.runtime->sculpt_session;

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  if (!ss) {
    return OPERATOR_CANCELLED;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  switch (bke::object::pbvh_get(ob)->type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(ob.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      attributes.remove(".sculpt_persistent_co");
      attributes.remove(".sculpt_persistent_no");
      attributes.remove(".sculpt_persistent_disp");

      const bke::AttributeReader positions = attributes.lookup<float3>("position");
      if (positions.sharing_info && positions.varray.is_span()) {
        attributes.add<float3>(
            ".sculpt_persistent_co",
            bke::AttrDomain::Point,
            bke::AttributeInitShared(positions.varray.get_internal_span().data(),
                                     *positions.sharing_info));
      }
      else {
        attributes.add<float3>(".sculpt_persistent_co",
                               bke::AttrDomain::Point,
                               bke::AttributeInitVArray(positions.varray));
      }

      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(*depsgraph, ob);
      attributes.add<float3>(".sculpt_persistent_no",
                             bke::AttrDomain::Point,
                             bke::AttributeInitVArray(VArray<float3>::from_span(vert_normals)));
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
      ss->persistent.sculpt_persistent_co = subdiv_ccg.positions;
      ss->persistent.sculpt_persistent_no = subdiv_ccg.normals;
      ss->persistent.sculpt_persistent_disp = Array<float>(subdiv_ccg.positions.size(), 0.0f);
      ss->persistent.grid_size = subdiv_ccg.grid_size;
      ss->persistent.grids_num = subdiv_ccg.grids_num;
      break;
    }
    case bke::pbvh::Type::BMesh: {
      return OPERATOR_CANCELLED;
    }
  }

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_set_persistent_base(wmOperatorType *ot)
{
  ot->name = "Set Persistent Base";
  ot->idname = "SCULPT_OT_set_persistent_base";
  ot->description = "Reset the copy of the mesh that is being sculpted on";

  ot->exec = set_persistent_base_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Optimize Operator
 * \{ */

static wmOperatorStatus optimize_exec(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);

  BKE_sculptsession_free_pbvh(ob);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);

  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);

  return OPERATOR_FINISHED;
}

/* The BVH gets less optimal more quickly with dynamic topology than
 * regular sculpting. There is no doubt more clever stuff we can do to
 * optimize it on the fly, but for now this gives the user a nicer way
 * to recalculate it than toggling modes. */
static void SCULPT_OT_optimize(wmOperatorType *ot)
{
  ot->name = "Rebuild BVH";
  ot->idname = "SCULPT_OT_optimize";
  ot->description = "Recalculate the sculpt BVH to improve performance";

  ot->exec = optimize_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Symmetrize Operator
 * \{ */

static bool no_multires_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return false;
  }
  if (ob->type != OB_MESH) {
    return false;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*ob);
  if (sculpt_mode_poll(C) && ob->runtime->sculpt_session && pbvh) {
    return pbvh->type() != bke::pbvh::Type::Grids;
  }
  return false;
}

static wmOperatorStatus symmetrize_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  const Scene &scene = *CTX_data_scene(C);
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  const float dist = RNA_float_get(op->ptr, "merge_tolerance");

  if (!pbvh) {
    return OPERATOR_CANCELLED;
  }

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  switch (pbvh->type()) {
    case bke::pbvh::Type::BMesh: {
      /* Dyntopo Symmetrize. */

      /* To simplify undo for symmetrize, all BMesh elements are logged
       * as deleted, then after symmetrize operation all BMesh elements
       * are logged as added (as opposed to attempting to store just the
       * parts that symmetrize modifies). */
      undo::push_begin(scene, ob, op);
      undo::push_node(depsgraph, ob, nullptr, undo::Type::Geometry);
      BM_log_before_all_removed(ss.bm, ss.bm_log);

      BM_mesh_toolflags_set(ss.bm, true);

      /* Symmetrize and re-triangulate. */
      BMO_op_callf(ss.bm,
                   (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
                   "symmetrize input=%avef direction=%i dist=%f use_shapekey=%b",
                   sd.symmetrize_direction,
                   dist,
                   true);
      dyntopo::triangulate(ss.bm);

      /* Bisect operator flags edges (keep tags clean for edge queue). */
      BM_mesh_elem_hflag_disable_all(ss.bm, BM_EDGE, BM_ELEM_TAG, false);

      BM_mesh_toolflags_set(ss.bm, false);

      /* Finish undo. */
      BM_log_all_added(ss.bm, ss.bm_log);
      undo::push_end(ob);

      break;
    }
    case bke::pbvh::Type::Mesh: {
      /* Mesh Symmetrize. */
      undo::geometry_begin(scene, ob, op);
      Mesh *mesh = id_cast<Mesh *>(ob.data);

      BKE_mesh_mirror_apply_mirror_on_axis(bmain, mesh, sd.symmetrize_direction, dist);

      undo::geometry_end(ob);
      BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);

      break;
    }
    case bke::pbvh::Type::Grids:
      return OPERATOR_CANCELLED;
  }

  BKE_sculptsession_free_pbvh(ob);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_symmetrize(wmOperatorType *ot)
{
  ot->name = "Symmetrize";
  ot->idname = "SCULPT_OT_symmetrize";
  ot->description = "Symmetrize the topology modifications";

  ot->exec = symmetrize_exec;
  ot->poll = no_multires_poll;

  PropertyRNA *prop = RNA_def_float(ot->srna,
                                    "merge_tolerance",
                                    0.0005f,
                                    0.0f,
                                    std::numeric_limits<float>::max(),
                                    "Merge Distance",
                                    "Distance within which symmetrical vertices are merged",
                                    0.0f,
                                    1.0f);

  RNA_def_property_ui_range(prop, 0.0, std::numeric_limits<float>::max(), 0.001, 5);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Mode Toggle Operator
 * \{ */

static void init_sculpt_mode_session(Main &bmain, Depsgraph &depsgraph, Scene &scene, Object &ob)
{
  /* Create persistent sculpt mode data. */
  BKE_sculpt_toolsettings_data_ensure(&bmain, &scene);

  /* Create sculpt mode session data. */
  if (ob.runtime->sculpt_session != nullptr) {
    BKE_sculptsession_free(&ob);
  }
  ob.runtime->sculpt_session = MEM_new<SculptSession>(__func__);
  ob.runtime->sculpt_session->mode_type = OB_MODE_SCULPT;

  /* Trigger evaluation of modifier stack to ensure
   * multires modifier sets .runtime.ccg in
   * the evaluated mesh.
   */
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);

  BKE_scene_graph_evaluated_ensure(&depsgraph, &bmain);

  /* This function expects a fully evaluated depsgraph. */
  BKE_sculpt_update_object_for_edit(&depsgraph, &ob, false);

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (mesh.attributes().contains(".sculpt_face_set")) {
    /* Here we can detect geometry that was just added to Sculpt Mode as it has the
     * face_set_none assigned, so we can create a new face set for it. */
    /* In sculpt mode all geometry that is assigned to face_set_none is considered as not
     * initialized, which is used is some operators that modify the mesh topology to perform
     * certain actions in the new faces. After these operations are finished, all faces should have
     * a valid face set ID assigned (different from face_set_none) to manage their
     * visibility correctly. */
    /* TODO(pablodp606): Based on this we can improve the UX in future tools for creating new
     * objects, like moving the transform pivot position to the new area or masking existing
     * geometry. */
    const int new_face_set = face_set::find_next_available_id(ob);
    face_set::initialize_none_to_id(id_cast<Mesh *>(ob.data), new_face_set);
  }
}

void ensure_valid_pivot(const Object &ob, Paint &paint)
{
  bke::PaintRuntime &paint_runtime = *paint.runtime;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);

  /* Account for the case where no objects are evaluated. */
  if (!pbvh) {
    return;
  }

  /* No valid pivot? Use bounding box center. */
  if (paint_runtime.average_stroke_counter == 0 || !paint_runtime.last_stroke_valid) {
    const Bounds<float3> bounds = bke::pbvh::bounds_get(*pbvh);
    const float3 center = math::midpoint(bounds.min, bounds.max);
    const float3 location = math::transform_point(ob.object_to_world(), center);

    copy_v3_v3(paint_runtime.average_stroke_accum, location);
    paint_runtime.average_stroke_counter = 1;

    /* Update last stroke position. */
    paint_runtime.last_stroke_valid = true;
  }
}

void object_sculpt_mode_enter(Main &bmain,
                              Depsgraph &depsgraph,
                              Scene &scene,
                              Object &ob,
                              const bool force_dyntopo,
                              ReportList *reports)
{
  const eObjectMode mode_flag = OB_MODE_SCULPT;
  Mesh *mesh = BKE_mesh_from_object(&ob);

  /* Re-triangulating the mesh for position changes in sculpt mode isn't worth the performance
   * impact, so delay triangulation updates until the user exits sculpt mode. */
  mesh->runtime->corner_tris_cache.freeze();

  /* Enter sculpt mode. */
  ob.mode |= mode_flag;

  init_sculpt_mode_session(bmain, depsgraph, scene, ob);

  if (!(fabsf(ob.scale[0] - ob.scale[1]) < 1e-4f && fabsf(ob.scale[1] - ob.scale[2]) < 1e-4f)) {
    BKE_report(
        reports, RPT_WARNING, "Object has non-uniform scale, sculpting may be unpredictable");
  }
  else if (is_negative_m4(ob.object_to_world().ptr())) {
    BKE_report(reports, RPT_WARNING, "Object has negative scale, sculpting may be unpredictable");
  }

  if (USER_EXPERIMENTAL_TEST(&U, use_sculpt_texture_paint)) {
    BKE_texpaint_slots_refresh_object(&scene, &ob);

    PaintModeSettings *paint_settings = &scene.toolsettings->paint_mode;
    Image *image;
    ImageUser *image_user;

    if (BKE_paint_canvas_image_get(paint_settings, &ob, &image, &image_user)) {
      ED_space_image_sync(&bmain, image, false);
    }
  }

  Paint *paint = BKE_paint_get_active_from_paintmode(&scene, PaintMode::Sculpt);
  BKE_paint_init(&bmain, &scene, PaintMode::Sculpt);

  ED_paint_cursor_start(paint, brush_cursor_poll);

  /* Check dynamic-topology flag; re-enter dynamic-topology mode when changing modes,
   * As long as no data was added that is not supported. */
  if (mesh->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) {
    MultiresModifierData *mmd = BKE_sculpt_multires_active(&scene, &ob);

    const char *message_unsupported = nullptr;
    if (mesh->corners_num != mesh->faces_num * 3) {
      message_unsupported = RPT_("non-triangle face");
    }
    else if (mmd != nullptr) {
      message_unsupported = RPT_("multi-res modifier");
    }
    else {
      const dyntopo::WarnFlag flag = dyntopo::check_attribute_warning(scene, ob);
      if (flag == 0) {
        /* pass */
      }
      else if (flag & dyntopo::ATTRIBUTES) {
        BKE_report(reports,
                   RPT_WARNING,
                   "Dyntopo will not preserve face sets, colors, UVs, or other attributes");
      }
      else if (flag & dyntopo::MODIFIER) {
        message_unsupported = RPT_("constructive modifier");
      }
      else {
        BLI_assert_unreachable();
      }
    }

    if ((message_unsupported == nullptr) || force_dyntopo) {
      /* Needed because we may be entering this mode before the undo system loads. */
      wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
      const bool has_undo = wm->runtime->undo_stack != nullptr;
      /* Undo push is needed to prevent memory leak. */
      if (has_undo) {
        undo::push_begin_ex(scene, ob, "Dynamic topology enable");
      }
      dyntopo::enable_ex(bmain, depsgraph, ob);
      if (has_undo) {
        undo::push_node(depsgraph, ob, nullptr, undo::Type::DyntopoBegin);
        undo::push_end(ob);
      }
    }
    else {
      BKE_reportf(
          reports, RPT_WARNING, "Dynamic Topology found: %s, disabled", message_unsupported);
      mesh->flag &= ~ME_SCULPT_DYNAMIC_TOPOLOGY;
    }
  }

  ensure_valid_pivot(ob, *paint);

  /* Flush object mode. */
  DEG_id_tag_update(&ob.id, ID_RECALC_SYNC_TO_EVAL);
}

void object_sculpt_mode_enter(bContext *C, Depsgraph &depsgraph, ReportList *reports)
{
  Main &bmain = *CTX_data_main(C);
  Scene &scene = *CTX_data_scene(C);
  ViewLayer &view_layer = *CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  Object &ob = *BKE_view_layer_active_object_get(&view_layer);
  object_sculpt_mode_enter(bmain, depsgraph, scene, ob, false, reports);
}

bool object_sculpt_mode_exit(Main &bmain, Depsgraph &depsgraph, Scene &scene, Object &ob)
{
  const eObjectMode mode_flag = OB_MODE_SCULPT;
  Mesh *mesh = BKE_mesh_from_object(&ob);

  if (!multires_flush_sculpt_updates(&ob)) {
    return false;
  }
  mesh->runtime->corner_tris_cache.unfreeze();

  /* Not needed for now. */
#if 0
  MultiresModifierData *mmd = BKE_sculpt_multires_active(scene, ob);
  const int flush_recalc = ed_object_sculptmode_flush_recalc_flag(scene, ob, mmd);
#endif

  /* Always for now, so leaving sculpt mode always ensures scene is in
   * a consistent state. */
  if (true || /* flush_recalc || */ (ob.runtime->sculpt_session && ob.runtime->sculpt_session->bm))
  {
    DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  }

  if (mesh->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) {
    /* Dynamic topology must be disabled before exiting sculpt
     * mode to ensure the undo stack stays in a consistent
     * state. */
    dyntopo::disable_with_undo(bmain, depsgraph, scene, ob);

    /* Store so we know to re-enable when entering sculpt mode. */
    mesh->flag |= ME_SCULPT_DYNAMIC_TOPOLOGY;
  }

  /* Leave sculpt mode. */
  ob.mode &= ~mode_flag;

  BKE_sculptsession_free(&ob);

  paint_cursor_delete_textures();

  /* Never leave derived meshes behind. */
  BKE_object_free_derived_caches(&ob);

  /* Flush object mode. */
  DEG_id_tag_update(&ob.id, ID_RECALC_SYNC_TO_EVAL);
  return true;
}

bool object_sculpt_mode_exit(bContext *C, Depsgraph &depsgraph)
{
  Main &bmain = *CTX_data_main(C);
  Scene &scene = *CTX_data_scene(C);
  ViewLayer &view_layer = *CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  Object &ob = *BKE_view_layer_active_object_get(&view_layer);
  return object_sculpt_mode_exit(bmain, depsgraph, scene, ob);
}

static wmOperatorStatus sculpt_mode_toggle_exec(bContext *C, wmOperator *op)
{
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  Main &bmain = *CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
  Scene &scene = *CTX_data_scene(C);
  ToolSettings &ts = *scene.toolsettings;
  ViewLayer &view_layer = *CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  Object &ob = *BKE_view_layer_active_object_get(&view_layer);
  const eObjectMode mode_flag = OB_MODE_SCULPT;
  const bool is_mode_set = (ob.mode & mode_flag) != 0;

  if (!is_mode_set) {
    if (!object::mode_compat_set(C, &ob, eObjectMode(mode_flag), op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }

  if (is_mode_set) {
    if (!object_sculpt_mode_exit(bmain, *depsgraph, scene, ob)) {
      BKE_report(op->reports, RPT_ERROR, "Cannot leave Sculpt Mode: layer changes could not be recorded");
      return OPERATOR_CANCELLED;
    }
  }
  else {
    if (depsgraph) {
      depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    }
    object_sculpt_mode_enter(bmain, *depsgraph, scene, ob, false, op->reports);
    BKE_paint_brushes_validate(&bmain, &ts.sculpt->paint);

    if (ob.mode & mode_flag) {
      Mesh *mesh = id_cast<Mesh *>(ob.data);
      /* Dyntopo adds its own undo step. */
      if ((mesh->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) == 0) {
        /* Without this the memfile undo step is used,
         * while it works it causes lag when undoing the first undo step, see #71564. */
        wmWindowManager *wm = CTX_wm_manager(C);
        if (wm->op_undo_depth <= 1) {
          undo::push_enter_sculpt_mode(scene, ob, op);
          undo::push_end(ob);
        }
      }
    }
  }

  WM_event_add_notifier(C, NC_SCENE | ND_MODE, &scene);

  WM_msg_publish_rna_prop(mbus, &ob.id, &ob, Object, mode);

  WM_toolsystem_update_from_context_view3d(C);

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_sculptmode_toggle(wmOperatorType *ot)
{
  ot->name = "Sculpt Mode";
  ot->idname = "SCULPT_OT_sculptmode_toggle";
  ot->description = "Toggle sculpt mode in 3D view";

  ot->exec = sculpt_mode_toggle_exec;
  ot->poll = ED_operator_object_active_editable_mesh_from_view_layer;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

namespace mask {

/* -------------------------------------------------------------------- */
/** \name Mask By Color
 * \{ */

/**
 * #sculpt_mask_by_color_delta_get returns values in the (0,1) range that are used to generate the
 * mask based on the difference between two colors (the active color and the color of any other
 * vertex). Ideally, a threshold of 0 should mask only the colors that are equal to the active
 * color and threshold of 1 should mask all colors. In order to avoid artifacts and produce softer
 * falloffs in the mask, the MASK_BY_COLOR_SLOPE defines the size of the transition values between
 * masked and unmasked vertices. The smaller this value is, the sharper the generated mask is going
 * to be.
 */
#define MASK_BY_COLOR_SLOPE 0.25f

static float color_delta_get(const float3 &color_a,
                             const float3 &color_b,
                             const float threshold,
                             const bool invert)
{
  float len = math::distance(color_a, color_b);
  /* Normalize len to the (0, 1) range. */
  len = len / std::numbers::sqrt3_v<float>;

  if (len < threshold - MASK_BY_COLOR_SLOPE) {
    len = 1.0f;
  }
  else if (len >= threshold) {
    len = 0.0f;
  }
  else {
    len = (-len + threshold) / MASK_BY_COLOR_SLOPE;
  }

  if (invert) {
    return 1.0f - len;
  }
  return len;
}

static float final_mask_get(const float current_mask,
                            const float new_mask,
                            const bool invert,
                            const bool preserve_mask)
{
  if (preserve_mask) {
    if (invert) {
      return std::min(current_mask, new_mask);
    }
    return std::max(current_mask, new_mask);
  }
  return new_mask;
}

static void mask_by_color_contiguous_mesh(const Depsgraph &depsgraph,
                                          Object &object,
                                          const int vert,
                                          const float threshold,
                                          const bool invert,
                                          const bool preserve_mask)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan colors = *attributes.lookup_or_default<ColorGeometry4f>(
      mesh.active_color_attribute, bke::AttrDomain::Point, {});
  const float4 active_color = float4(colors[vert]);

  Array<float> new_mask(mesh.verts_num, invert ? 1.0f : 0.0f);

  flood_fill::FillDataMesh flood(mesh.verts_num);
  flood.add_initial(vert);

  flood.execute(object, vert_to_face_map, [&](int /*from_v*/, int to_v) {
    const float4 current_color = float4(colors[to_v]);

    float new_vertex_mask = color_delta_get(
        current_color.xyz(), active_color.xyz(), threshold, invert);
    new_mask[to_v] = new_vertex_mask;

    float len = math::distance(current_color.xyz(), active_color.xyz());
    len = len / std::numbers::sqrt3_v<float>;
    return len <= threshold;
  });

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  update_mask_mesh(
      depsgraph, object, node_mask, [&](MutableSpan<float> node_masks, const Span<int> verts) {
        for (const int i : verts.index_range()) {
          node_masks[i] = final_mask_get(node_masks[i], new_mask[verts[i]], invert, preserve_mask);
        }
      });
}

static void mask_by_color_full_mesh(const Depsgraph &depsgraph,
                                    Object &object,
                                    const int vert,
                                    const float threshold,
                                    const bool invert,
                                    const bool preserve_mask)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan colors = *attributes.lookup_or_default<ColorGeometry4f>(
      mesh.active_color_attribute, bke::AttrDomain::Point, {});
  const float4 active_color = float4(colors[vert]);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  update_mask_mesh(
      depsgraph, object, node_mask, [&](MutableSpan<float> node_masks, const Span<int> verts) {
        for (const int i : verts.index_range()) {
          const float4 current_color = float4(colors[verts[i]]);
          const float current_mask = node_masks[i];
          const float new_mask = color_delta_get(
              active_color.xyz(), current_color.xyz(), threshold, invert);
          node_masks[i] = final_mask_get(current_mask, new_mask, invert, preserve_mask);
        }
      });
}

static wmOperatorStatus mask_by_color(bContext *C, wmOperator *op, const float2 region_location)
{
  const Scene &scene = *CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  View3D *v3d = CTX_wm_view3d(C);

  {
    if (v3d && v3d->shading.type == OB_SOLID) {
      v3d->shading.color_type = V3D_SHADING_VERTEX_COLOR;
    }
  }

  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  ed::sculpt_paint::mask_overlay_check(*C, *op);

  /* Color data is not available in multi-resolution or dynamic topology. */
  if (!color_supported_check(scene, ob, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  /* Tools that are not brushes do not have the brush gizmo to update the vertex as the mouse move,
   * so it needs to be updated here. */
  cursor_geometry_info_update(C, region_location, false);

  if (std::holds_alternative<std::monostate>(ss.active_vert())) {
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(scene, ob, op);
  BKE_sculpt_color_layer_create_if_needed(&ob);

  const float threshold = RNA_float_get(op->ptr, "threshold");
  const bool invert = RNA_boolean_get(op->ptr, "invert");
  const bool preserve_mask = RNA_boolean_get(op->ptr, "preserve_previous_mask");

  const int active_vert = std::get<int>(ss.active_vert());
  if (RNA_boolean_get(op->ptr, "contiguous")) {
    mask_by_color_contiguous_mesh(*depsgraph, ob, active_vert, threshold, invert, preserve_mask);
  }
  else {
    mask_by_color_full_mesh(*depsgraph, ob, active_vert, threshold, invert, preserve_mask);
  }

  undo::push_end(ob);

  flush_update_done(C, ob, UpdateType::Mask);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus mask_by_color_exec(bContext *C, wmOperator *op)
{
  int2 mval;
  RNA_int_get_array(op->ptr, "location", mval);
  return mask_by_color(C, op, float2(mval[0], mval[1]));
}

static wmOperatorStatus mask_by_color_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RNA_int_set_array(op->ptr, "location", event->mval);
  return mask_by_color(C, op, float2(event->mval[0], event->mval[1]));
}

static void SCULPT_OT_mask_by_color(wmOperatorType *ot)
{
  ot->name = "Mask by Color";
  ot->idname = "SCULPT_OT_mask_by_color";
  ot->description = "Creates a mask based on the active color attribute";

  ot->invoke = mask_by_color_invoke;
  ot->exec = mask_by_color_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  ot->prop = RNA_def_boolean(
      ot->srna, "contiguous", false, "Contiguous", "Mask only contiguous color areas");

  ot->prop = RNA_def_boolean(ot->srna, "invert", false, "Invert", "Invert the generated mask");
  ot->prop = RNA_def_boolean(
      ot->srna,
      "preserve_previous_mask",
      false,
      "Preserve Previous Mask",
      "Preserve the previous mask and add or subtract the new one generated by the colors");

  RNA_def_float(ot->srna,
                "threshold",
                0.35f,
                0.0f,
                1.0f,
                "Threshold",
                "How much changes in color affect the mask generation",
                0.0f,
                1.0f);

  ot->prop = RNA_def_int_array(ot->srna,
                               "location",
                               2,
                               nullptr,
                               0,
                               SHRT_MAX,
                               "Location",
                               "Region coordinates of sampling",
                               0,
                               SHRT_MAX);
  RNA_def_property_flag(ot->prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask from Cavity
 * \{ */

enum class ApplyMaskMode : int8_t {
  Mix,
  Multiply,
  Divide,
  Add,
  Subtract,
};

static EnumPropertyItem mix_modes[] = {
    {int(ApplyMaskMode::Mix), "MIX", ICON_NONE, "Mix", ""},
    {int(ApplyMaskMode::Multiply), "MULTIPLY", ICON_NONE, "Multiply", ""},
    {int(ApplyMaskMode::Divide), "DIVIDE", ICON_NONE, "Divide", ""},
    {int(ApplyMaskMode::Add), "ADD", ICON_NONE, "Add", ""},
    {int(ApplyMaskMode::Subtract), "SUBTRACT", ICON_NONE, "Subtract", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class MaskSettingsSource : int8_t { Operator, Scene, Brush };

static EnumPropertyItem settings_sources[] = {
    {int(MaskSettingsSource::Operator),
     "OPERATOR",
     ICON_NONE,
     "Operator",
     "Use settings from operator properties"},
    {int(MaskSettingsSource::Brush), "BRUSH", ICON_NONE, "Brush", "Use settings from brush"},
    {int(MaskSettingsSource::Scene), "SCENE", ICON_NONE, "Scene", "Use settings from scene"},
    {0, nullptr, 0, nullptr, nullptr}};

struct LocalData {
  Vector<float> mask;
  Vector<float> factors;
  Vector<float> new_mask;
};

static void calc_new_masks(const ApplyMaskMode mode,
                           const Span<float> node_mask,
                           const MutableSpan<float> new_mask)
{
  switch (mode) {
    case ApplyMaskMode::Mix:
      break;
    case ApplyMaskMode::Multiply:
      for (const int i : node_mask.index_range()) {
        new_mask[i] = node_mask[i] * new_mask[i];
      }
      break;
    case ApplyMaskMode::Divide:
      for (const int i : node_mask.index_range()) {
        new_mask[i] = new_mask[i] > 0.00001f ? node_mask[i] / new_mask[i] : 0.0f;
      }
      break;
    case ApplyMaskMode::Add:
      for (const int i : node_mask.index_range()) {
        new_mask[i] = node_mask[i] + new_mask[i];
      }
      break;
    case ApplyMaskMode::Subtract:
      for (const int i : node_mask.index_range()) {
        new_mask[i] = node_mask[i] - new_mask[i];
      }
      break;
  }
  mask::clamp_mask(new_mask);
}

static void apply_mask_mesh(const Depsgraph &depsgraph,
                            const Object &object,
                            const Span<bool> hide_vert,
                            const auto_mask::Cache &automasking,
                            const ApplyMaskMode mode,
                            const float factor,
                            const bke::pbvh::MeshNode &node,
                            LocalData &tls,
                            const MutableSpan<float> mask)
{
  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide(hide_vert, verts, factors);
  scale_factors(factors, factor);

  tls.new_mask.resize(verts.size());
  const MutableSpan<float> new_mask = tls.new_mask;
  new_mask.fill(1.0f);
  auto_mask::calc_vert_factors(depsgraph, object, automasking, node, verts, new_mask);

  mask::invert_mask(new_mask);

  tls.mask.resize(verts.size());
  const MutableSpan<float> node_mask = tls.mask;
  gather_data_mesh(mask.as_span(), verts, node_mask);

  calc_new_masks(mode, node_mask, new_mask);
  mix_new_masks(new_mask, factors, node_mask);

  scatter_data_mesh(node_mask.as_span(), verts, mask);
}

static void apply_mask_grids(const Depsgraph &depsgraph,
                             Object &object,
                             const auto_mask::Cache &automasking,
                             const ApplyMaskMode mode,
                             const float factor,
                             const bke::pbvh::GridsNode &node,
                             LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  const Span<int> grids = node.grids();
  const int grid_verts_num = grids.size() * key.grid_area;

  tls.factors.resize(grid_verts_num);
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide(subdiv_ccg, grids, factors);
  scale_factors(factors, factor);

  tls.new_mask.resize(grid_verts_num);
  const MutableSpan<float> new_mask = tls.new_mask;
  new_mask.fill(1.0f);
  auto_mask::calc_grids_factors(depsgraph, object, automasking, node, grids, new_mask);

  mask::invert_mask(new_mask);

  tls.mask.resize(grid_verts_num);
  const MutableSpan<float> node_mask = tls.mask;
  gather_mask_grids(subdiv_ccg, grids, node_mask);

  calc_new_masks(mode, node_mask, new_mask);
  mix_new_masks(new_mask, factors, node_mask);

  scatter_mask_grids(node_mask.as_span(), subdiv_ccg, grids);
}

static void apply_mask_bmesh(const Depsgraph &depsgraph,
                             Object &object,
                             const auto_mask::Cache &automasking,
                             const ApplyMaskMode mode,
                             const float factor,
                             bke::pbvh::BMeshNode &node,
                             LocalData &tls)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide(verts, factors);
  scale_factors(factors, factor);

  tls.new_mask.resize(verts.size());
  const MutableSpan<float> new_mask = tls.new_mask;
  new_mask.fill(1.0f);
  auto_mask::calc_vert_factors(depsgraph, object, automasking, node, verts, new_mask);

  mask::invert_mask(new_mask);

  tls.mask.resize(verts.size());
  const MutableSpan<float> node_mask = tls.mask;
  gather_mask_bmesh(*ss.bm, verts, node_mask);

  calc_new_masks(mode, node_mask, new_mask);
  mix_new_masks(new_mask, factors, node_mask);

  scatter_mask_bmesh(node_mask.as_span(), *ss.bm, verts);
}

static void apply_mask_from_settings(const Depsgraph &depsgraph,
                                     Object &object,
                                     bke::pbvh::Tree &pbvh,
                                     const IndexMask &node_mask,
                                     const auto_mask::Cache &automasking,
                                     const ApplyMaskMode mode,
                                     const float factor)
{
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      bke::SpanAttributeWriter mask = attributes.lookup_or_add_for_write_span<float>(
          ".sculpt_mask", bke::AttrDomain::Point);
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            apply_mask_mesh(
                depsgraph, object, hide_vert, automasking, mode, factor, nodes[i], tls, mask.span);
            bke::pbvh::node_update_mask_mesh(mask.span, nodes[i]);
          },
          exec_mode::grain_size(1));
      mask.finish();
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      MutableSpan<float> masks = subdiv_ccg.masks;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            apply_mask_grids(depsgraph, object, automasking, mode, factor, nodes[i], tls);
            bke::pbvh::node_update_mask_grids(key, masks, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const int mask_offset = CustomData_get_offset_named(
          &object.runtime->sculpt_session->bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            apply_mask_bmesh(depsgraph, object, automasking, mode, factor, nodes[i], tls);
            bke::pbvh::node_update_mask_bmesh(mask_offset, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
}

static wmOperatorStatus mask_from_cavity_exec(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Paint &paint = sd.paint;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  MultiresModifierData *mmd = BKE_sculpt_multires_active(CTX_data_scene(C), &ob);
  BKE_sculpt_mask_layers_ensure(depsgraph, CTX_data_main(C), &ob, mmd);

  ed::sculpt_paint::mask_overlay_check(*C, *op);

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);
  vert_random_access_ensure(ob);

  const ApplyMaskMode mode = ApplyMaskMode(RNA_enum_get(op->ptr, "mix_mode"));
  const float factor = RNA_float_get(op->ptr, "mix_factor");

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  /* Set up automasking settings. */
  Paint scene_copy = dna::shallow_copy(sd.paint);
  /* We don't do a deep copy of the automasking settings, we simply need a new one so that the
   * canonical pointer isn't overwritten. */
  MeshAutomaskingSettings automasking_settings;
  scene_copy.mesh_automasking_settings = &automasking_settings;

  /* TODO: This pattern of recreating the scene / brush and using them as the "settings" is weak
   * and can cause hard to find bugs due to modifying actual data. This should be refactored to
   * take in a options struct */
  MaskSettingsSource src = MaskSettingsSource(RNA_enum_get(op->ptr, "settings_source"));
  switch (src) {
    case MaskSettingsSource::Operator:
      if (RNA_boolean_get(op->ptr, "invert")) {
        scene_copy.mesh_automasking_settings->flags = BRUSH_AUTOMASKING_CAVITY_INVERTED;
      }
      else {
        scene_copy.mesh_automasking_settings->flags = BRUSH_AUTOMASKING_CAVITY_NORMAL;
      }

      if (RNA_boolean_get(op->ptr, "use_curve")) {
        scene_copy.mesh_automasking_settings->flags |= BRUSH_AUTOMASKING_CAVITY_USE_CURVE;
      }

      scene_copy.mesh_automasking_settings->cavity_blur_steps = RNA_int_get(op->ptr, "blur_steps");
      scene_copy.mesh_automasking_settings->cavity_factor = RNA_float_get(op->ptr, "factor");

      scene_copy.mesh_automasking_settings->cavity_curve =
          paint.mesh_automasking_settings->cavity_curve_op;
      break;
    case MaskSettingsSource::Brush:
      if (brush) {
        scene_copy.mesh_automasking_settings->flags = brush->mesh_automasking_settings->flags;
        scene_copy.mesh_automasking_settings->cavity_factor =
            brush->mesh_automasking_settings->cavity_factor;
        scene_copy.mesh_automasking_settings->cavity_curve =
            brush->mesh_automasking_settings->cavity_curve;
        scene_copy.mesh_automasking_settings->cavity_blur_steps =
            brush->mesh_automasking_settings->cavity_blur_steps;

        /* Ensure only cavity masking is enabled. */
        scene_copy.mesh_automasking_settings->flags &= BRUSH_AUTOMASKING_CAVITY_ALL |
                                                       BRUSH_AUTOMASKING_CAVITY_USE_CURVE;
      }
      else {
        scene_copy.mesh_automasking_settings->flags = 0;
        BKE_report(op->reports, RPT_WARNING, "No active brush");

        return OPERATOR_CANCELLED;
      }

      break;
    case MaskSettingsSource::Scene:
      /* Ensure only cavity masking is enabled. */
      scene_copy.mesh_automasking_settings->flags &= BRUSH_AUTOMASKING_CAVITY_ALL |
                                                     BRUSH_AUTOMASKING_CAVITY_USE_CURVE;
      break;
  }

  /* Ensure cavity mask is actually enabled. */
  if (!(scene_copy.mesh_automasking_settings->flags & BRUSH_AUTOMASKING_CAVITY_ALL)) {
    scene_copy.mesh_automasking_settings->flags |= BRUSH_AUTOMASKING_CAVITY_NORMAL;
  }

  /* Create copy of brush with cleared automasking settings. */
  Brush brush_copy = dna::shallow_copy(*brush);
  MeshAutomaskingSettings brush_settings;
  brush_settings.flags = 0;
  brush_settings.boundary_edges_propagation_steps = 1;
  brush_settings.cavity_curve = scene_copy.mesh_automasking_settings->cavity_curve;

  brush_copy.mesh_automasking_settings = &brush_settings;
  /* Set a brush type that doesn't change topology so automasking isn't "disabled". */
  brush_copy.sculpt_brush_type = SCULPT_BRUSH_TYPE_SMOOTH;

  std::unique_ptr<auto_mask::Cache> automasking = auto_mask::cache_init(
      *depsgraph, scene_copy, &brush_copy, ob);

  if (!automasking) {
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(scene, ob, op);
  undo::push_nodes(*depsgraph, ob, node_mask, undo::Type::Mask);

  automasking->calc_cavity_factor(*depsgraph, ob, node_mask);
  apply_mask_from_settings(*depsgraph, ob, pbvh, node_mask, *automasking, mode, factor);

  undo::push_end(ob);

  pbvh.tag_masks_changed(node_mask);
  flush_update_done(C, ob, UpdateType::Mask);
  tag_update_overlays(C);

  return OPERATOR_FINISHED;
}

static void mask_from_cavity_ui(bContext *C, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  Scene *scene = CTX_data_scene(C);
  Sculpt *sd = scene->toolsettings ? scene->toolsettings->sculpt : nullptr;

  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  MaskSettingsSource source = MaskSettingsSource(RNA_enum_get(op->ptr, "settings_source"));

  if (!sd) {
    source = MaskSettingsSource::Operator;
  }

  switch (source) {
    case MaskSettingsSource::Operator: {
      layout.prop(op->ptr, "mix_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "mix_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "blur_steps", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "invert", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "use_curve", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      if (sd && RNA_boolean_get(op->ptr, "use_curve")) {
        PointerRNA sculpt_ptr = RNA_pointer_create_discrete(&scene->id, RNA_Sculpt, sd);
        template_curve_mapping(&layout,
                               &sculpt_ptr,
                               "automasking_cavity_curve_op",
                               'v',
                               false,
                               false,
                               false,
                               false,
                               false);
      }
      break;
    }
    case MaskSettingsSource::Brush:
    case MaskSettingsSource::Scene:
      layout.prop(op->ptr, "mix_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "mix_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      break;
  }
}

static void SCULPT_OT_mask_from_cavity(wmOperatorType *ot)
{
  ot->name = "Mask From Cavity";
  ot->idname = "SCULPT_OT_mask_from_cavity";
  ot->description = "Creates a mask based on the curvature of the surface";

  ot->ui = mask_from_cavity_ui;
  ot->exec = mask_from_cavity_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "mix_mode", mix_modes, int(ApplyMaskMode::Mix), "Mode", "Mix mode");
  RNA_def_float(ot->srna, "mix_factor", 1.0f, 0.0f, 5.0f, "Mix Factor", "", 0.0f, 1.0f);
  RNA_def_enum(ot->srna,
               "settings_source",
               settings_sources,
               int(MaskSettingsSource::Operator),
               "Settings",
               "Use settings from here");
  RNA_def_float(ot->srna,
                "factor",
                0.5f,
                0.0f,
                5.0f,
                "Factor",
                "The contrast of the cavity mask",
                0.0f,
                1.0f);
  RNA_def_int(ot->srna,
              "blur_steps",
              2,
              0,
              25,
              "Blur",
              "The number of times the cavity mask is blurred",
              0,
              25);
  RNA_def_boolean(ot->srna, "use_curve", false, "Custom Curve", "");
  RNA_def_boolean(ot->srna, "invert", false, "Cavity (Inverted)", "");
}

enum class MaskBoundaryMode : int8_t { Mesh, FaceSets };

static wmOperatorStatus mask_from_boundary_exec(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Scene &scene = *CTX_data_scene(C);
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  MultiresModifierData *mmd = BKE_sculpt_multires_active(CTX_data_scene(C), &ob);
  BKE_sculpt_mask_layers_ensure(depsgraph, CTX_data_main(C), &ob, mmd);

  ed::sculpt_paint::mask_overlay_check(*C, *op);

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);
  vert_random_access_ensure(ob);

  const ApplyMaskMode mode = ApplyMaskMode(RNA_enum_get(op->ptr, "mix_mode"));
  const float factor = RNA_float_get(op->ptr, "mix_factor");

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  /* Set up automasking settings. */
  Paint scene_copy = dna::shallow_copy(sd.paint);
  /* We don't do a deep copy of the automasking settings, we simply need a new one so that the
   * canonical pointer isn't overwritten. */
  MeshAutomaskingSettings automasking_settings;
  scene_copy.mesh_automasking_settings = &automasking_settings;

  MaskSettingsSource src = MaskSettingsSource(RNA_enum_get(op->ptr, "settings_source"));
  switch (src) {
    case MaskSettingsSource::Operator: {
      const MaskBoundaryMode boundary_mode = MaskBoundaryMode(
          RNA_enum_get(op->ptr, "boundary_mode"));
      switch (boundary_mode) {
        case MaskBoundaryMode::Mesh:
          scene_copy.mesh_automasking_settings->flags = BRUSH_AUTOMASKING_BOUNDARY_EDGES;
          break;
        case MaskBoundaryMode::FaceSets:
          scene_copy.mesh_automasking_settings->flags = BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS;
          break;
      }
      scene_copy.mesh_automasking_settings->boundary_edges_propagation_steps = RNA_int_get(
          op->ptr, "propagation_steps");
      break;
    }
    case MaskSettingsSource::Brush:
      if (brush) {
        scene_copy.mesh_automasking_settings->flags = brush->mesh_automasking_settings->flags;
        scene_copy.mesh_automasking_settings->boundary_edges_propagation_steps =
            brush->mesh_automasking_settings->boundary_edges_propagation_steps;

        scene_copy.mesh_automasking_settings->flags &= BRUSH_AUTOMASKING_BOUNDARY_EDGES |
                                                       BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS;
      }
      else {
        scene_copy.mesh_automasking_settings->flags = 0;
        BKE_report(op->reports, RPT_WARNING, "No active brush");

        return OPERATOR_CANCELLED;
      }

      break;
    case MaskSettingsSource::Scene:
      scene_copy.mesh_automasking_settings->flags &= BRUSH_AUTOMASKING_BOUNDARY_EDGES |
                                                     BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS;
      break;
  }

  /* Create copy of brush with cleared automasking settings. */
  Brush brush_copy = dna::shallow_copy(*brush);
  MeshAutomaskingSettings brush_settings;
  brush_settings.flags = 0;
  brush_settings.boundary_edges_propagation_steps = 1;
  /* Set a brush type that doesn't change topology so automasking isn't "disabled". */
  brush_copy.mesh_automasking_settings = &brush_settings;
  brush_copy.sculpt_brush_type = SCULPT_BRUSH_TYPE_SMOOTH;

  std::unique_ptr<auto_mask::Cache> automasking = auto_mask::cache_init(
      *depsgraph, scene_copy, &brush_copy, ob);

  if (!automasking) {
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(scene, ob, op);
  undo::push_nodes(*depsgraph, ob, node_mask, undo::Type::Mask);

  apply_mask_from_settings(*depsgraph, ob, pbvh, node_mask, *automasking, mode, factor);

  undo::push_end(ob);

  pbvh.tag_masks_changed(node_mask);
  flush_update_done(C, ob, UpdateType::Mask);
  tag_update_overlays(C);

  return OPERATOR_FINISHED;
}

static void mask_from_boundary_ui(bContext *C, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  Scene *scene = CTX_data_scene(C);
  Sculpt *sd = scene->toolsettings ? scene->toolsettings->sculpt : nullptr;

  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  MaskSettingsSource source = MaskSettingsSource(RNA_enum_get(op->ptr, "settings_source"));

  if (!sd) {
    source = MaskSettingsSource::Operator;
  }

  switch (source) {
    case MaskSettingsSource::Operator: {
      layout.prop(op->ptr, "mix_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "mix_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "boundary_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "propagation_steps", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    }
    case MaskSettingsSource::Brush:
    case MaskSettingsSource::Scene:
      layout.prop(op->ptr, "mix_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "mix_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
  }
}

static void SCULPT_OT_mask_from_boundary(wmOperatorType *ot)
{
  ot->name = "Mask From Boundary";
  ot->idname = "SCULPT_OT_mask_from_boundary";
  ot->description = "Creates a mask based on the boundaries of the surface";

  ot->ui = mask_from_boundary_ui;
  ot->exec = mask_from_boundary_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "mix_mode", mix_modes, int(ApplyMaskMode::Mix), "Mode", "Mix mode");
  RNA_def_float(ot->srna, "mix_factor", 1.0f, 0.0f, 5.0f, "Mix Factor", "", 0.0f, 1.0f);
  RNA_def_enum(ot->srna,
               "settings_source",
               settings_sources,
               int(MaskSettingsSource::Operator),
               "Settings",
               "Use settings from here");

  static EnumPropertyItem mask_boundary_modes[] = {
      {int(MaskBoundaryMode::Mesh),
       "MESH",
       ICON_NONE,
       "Mesh",
       "Calculate the boundary mask based on disconnected mesh topology islands"},
      {int(MaskBoundaryMode::FaceSets),
       "FACE_SETS",
       ICON_NONE,
       "Face Sets",
       "Calculate the boundary mask between face sets"},
      {0, nullptr, 0, nullptr, nullptr}};

  RNA_def_enum(ot->srna,
               "boundary_mode",
               mask_boundary_modes,
               int(MaskBoundaryMode::Mesh),
               "Mode",
               "Boundary type to mask");
  RNA_def_int(ot->srna, "propagation_steps", 1, 1, 20, "Propagation Steps", "", 1, 20);
}

/** \} */

}  // namespace mask

void operatortypes_sculpt()
{
  WM_operatortype_append(SCULPT_OT_brush_stroke);
  WM_operatortype_append(SCULPT_OT_sculptmode_toggle);
  WM_operatortype_append(SCULPT_OT_set_persistent_base);
  WM_operatortype_append(dyntopo::SCULPT_OT_dynamic_topology_toggle);
  WM_operatortype_append(SCULPT_OT_optimize);
  WM_operatortype_append(SCULPT_OT_symmetrize);
  WM_operatortype_append(dyntopo::SCULPT_OT_detail_flood_fill);
  WM_operatortype_append(dyntopo::SCULPT_OT_sample_detail_size);
  WM_operatortype_append(filter::SCULPT_OT_mesh_filter);
  WM_operatortype_append(mask::SCULPT_OT_mask_filter);
  WM_operatortype_append(SCULPT_OT_set_pivot_position);
  WM_operatortype_append(SCULPT_OT_multires_layer_add);
  WM_operatortype_append(SCULPT_OT_multires_layer_commit);
  WM_operatortype_append(SCULPT_OT_multires_layer_snapshot);
  WM_operatortype_append(SCULPT_OT_multires_base_capture);
  WM_operatortype_append(SCULPT_OT_multires_base_fingerprint);
  WM_operatortype_append(SCULPT_OT_multires_merge_visible_to_base);
  WM_operatortype_append(SCULPT_OT_multires_polypaint_attribute);
  WM_operatortype_append(SCULPT_OT_multires_texture_displacement_bake);
  WM_operatortype_append(SCULPT_OT_multires_shrinkwrap_bake);
  WM_operatortype_append(SCULPT_OT_multires_layer_delete);
  WM_operatortype_append(SCULPT_OT_multires_layer_duplicate);
  WM_operatortype_append(SCULPT_OT_multires_layer_set_active);
  WM_operatortype_append(SCULPT_OT_multires_layer_group_create);
  WM_operatortype_append(SCULPT_OT_multires_layer_group_add_active);
  WM_operatortype_append(SCULPT_OT_multires_layer_group_merge);
  WM_operatortype_append(SCULPT_OT_multires_layer_group_with_below);
  WM_operatortype_append(SCULPT_OT_multires_layer_group_remove);
  WM_operatortype_append(SCULPT_OT_multires_layer_group_visibility);
  WM_operatortype_append(SCULPT_OT_multires_layer_group_collapse);
  WM_operatortype_append(SCULPT_OT_multires_layer_move);
  WM_operatortype_append(SCULPT_OT_multires_layer_merge);
  WM_operatortype_append(SCULPT_OT_multires_layer_flatten);
  WM_operatortype_append(SCULPT_OT_multires_layer_solo);
  WM_operatortype_append(face_set::SCULPT_OT_face_sets_create);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_change_visibility);
  WM_operatortype_append(face_set::SCULPT_OT_face_sets_randomize_colors);
  WM_operatortype_append(face_set::SCULPT_OT_face_sets_init);
  WM_operatortype_append(face_set::SCULPT_OT_face_sets_edit);
  WM_operatortype_append(cloth::SCULPT_OT_cloth_filter);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_lasso_gesture);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_box_gesture);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_line_gesture);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_polyline_gesture);
  WM_operatortype_append(trim::SCULPT_OT_trim_box_gesture);
  WM_operatortype_append(trim::SCULPT_OT_trim_lasso_gesture);
  WM_operatortype_append(trim::SCULPT_OT_trim_line_gesture);
  WM_operatortype_append(trim::SCULPT_OT_trim_polyline_gesture);
  WM_operatortype_append(project::SCULPT_OT_project_line_gesture);

  WM_operatortype_append(color::SCULPT_OT_color_filter);
  WM_operatortype_append(mask::SCULPT_OT_mask_by_color);
  WM_operatortype_append(dyntopo::SCULPT_OT_dyntopo_detail_size_edit);
  WM_operatortype_append(mask::SCULPT_OT_mask_init);

  WM_operatortype_append(expand::SCULPT_OT_expand);
  WM_operatortype_append(mask::SCULPT_OT_mask_from_cavity);
  WM_operatortype_append(mask::SCULPT_OT_mask_from_boundary);
  WM_operatortype_append(SCULPT_OT_paint_mask_extract);
  WM_operatortype_append(SCULPT_OT_face_set_extract);
  WM_operatortype_append(SCULPT_OT_paint_mask_slice);
}

void keymap_sculpt(wmKeyConfig *keyconf)
{
  filter::modal_keymap(keyconf);
}

}  // namespace blender::ed::sculpt_paint
