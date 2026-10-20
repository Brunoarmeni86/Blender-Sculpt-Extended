/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* MULTIRES_HR_EXTRACT_V05: build selected quads only; no source writes.
 * CCG seam welding uses a topological canonical key, never spatial proximity. */
struct HRExtractBuffersV05 {
  Vector<float3> positions;
  Vector<int4> quads;
  Vector<int> face_sets;
};

static const char *hr_extract_build_v05(
    const int grids_num, const int side, const bool by_face_set, const int selected_id,
    const float threshold, const int max_vertices, const int max_faces, const bool reverse,
    const FunctionRef<int(int)> face_set_at, const FunctionRef<float(int)> mask_at,
    const FunctionRef<bool(int)> hidden_at, const FunctionRef<int(int)> canonical_at,
    const FunctionRef<float3(int)> position_at, HRExtractBuffersV05 &out)
{
  if (grids_num <= 0 || side < 2 || side > 4097 || max_vertices < 4 || max_faces < 1 ||
      !std::isfinite(threshold) || threshold < 0.0f || threshold > 1.0f ||
      int64_t(grids_num) * side * side > INT_MAX) {
    return "Invalid extraction layout or limits";
  }
  Map<int, int> vertex_by_key;
  const int area = side * side;
  const int total = grids_num * area;
  for (int grid = 0; grid < grids_num; grid++) {
    for (int y = 0; y < side - 1; y++) {
      for (int x = 0; x < side - 1; x++) {
        /* Match native CCG triangle winding: (v0,v2,v1), (v0,v3,v2). */
        const int samples[4] = {grid * area + y * side + x,
                               grid * area + (y + 1) * side + x,
                               grid * area + (y + 1) * side + x + 1,
                               grid * area + y * side + x + 1};
        bool keep = true;
        for (const int i : samples) {
          if (hidden_at(i)) {
            keep = false;
            break;
          }
          if (by_face_set) {
            if (face_set_at(i) != selected_id) {
              keep = false;
              break;
            }
          }
          else {
            const float mask = mask_at(i);
            if (!std::isfinite(mask)) {
              return "Non-finite sculpt mask";
            }
            if (mask < threshold) {
              keep = false;
              break;
            }
          }
        }
        if (!keep) {
          continue;
        }
        if (out.quads.size() >= max_faces) {
          return "Extraction face limit reached; lower the sculpt level or select a smaller region";
        }
        int4 quad;
        for (int corner = 0; corner < 4; corner++) {
          const int key = canonical_at(samples[corner]);
          if (key < 0 || key >= total) {
            return "Invalid CCG seam correspondence";
          }
          if (const int *existing = vertex_by_key.lookup_ptr(key)) {
            quad[corner] = *existing;
          }
          else {
            if (out.positions.size() >= max_vertices) {
              return "Extraction vertex limit reached; lower the sculpt level or select a smaller region";
            }
            const float3 position = position_at(key);
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
              return "Non-finite extracted position";
            }
            const int index = int(out.positions.size());
            out.positions.append(position);
            vertex_by_key.add(key, index);
            quad[corner] = index;
          }
        }
        if (quad[0] == quad[1] || quad[0] == quad[2] || quad[0] == quad[3] ||
            quad[1] == quad[2] || quad[1] == quad[3] || quad[2] == quad[3]) {
          return "Degenerate CCG topology; no object created";
        }
        if (reverse) {
          std::swap(quad[1], quad[3]);
        }
        out.quads.append(quad);
        out.face_sets.append(std::max(1, face_set_at(samples[0])));
      }
    }
  }
  return out.quads.is_empty() ? "No complete visible quad matches the selected mask or Face Set" : nullptr;
}

/* Included inside ed::sculpt_paint::face_set, after the read-only HR cursor helpers. */
static bool hr_extract_enabled_v05()
{
  const char *value = std::getenv("BLENDER_MULTIRES_HR_EXTRACT_V05");
  return rc10_tools_enabled() || (value && value[0] == '1' && value[1] == '\0');
}

static bool hr_extract_poll_v05(bContext *C)
{
  if (!hr_extract_enabled_v05() || !sculpt_mode_poll(C)) {
    return false;
  }
  const Object &object = *CTX_data_active_object(C);
  const SculptSession *ss = object.runtime->sculpt_session;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  return ss && ss->subdiv_ccg && ss->multires_modifier && !ss->bm && pbvh &&
         pbvh->type() == bke::pbvh::Type::Grids;
}

static int hr_extract_canonical_v05(const SubdivCCG &ccg, const CCGKey &key,
                                   const int index, SubdivCCGNeighbors &neighbors)
{
  const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, index);
  if (coord.x > 0 && coord.y > 0 && coord.x < key.grid_size - 1 && coord.y < key.grid_size - 1) {
    return index;
  }
  BKE_subdiv_ccg_neighbor_coords_get(ccg, coord, true, neighbors);
  int canonical = index;
  for (const SubdivCCGCoord &duplicate : neighbors.duplicates()) {
    canonical = std::min(canonical, duplicate.to_index(key));
  }
  return canonical;
}

static wmOperatorStatus hr_extract_execute_v05(bContext *C, wmOperator *op,
    const bool by_face_set,const int selected_id,const float threshold,
    const int max_vertices,const int max_faces,const bool add_solidify,const float thickness)
{
  Object &source = *CTX_data_active_object(C);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(&depsgraph, &source, false);
  if (!hr_extract_poll_v05(C)) {
    return OPERATOR_CANCELLED;
  }
  SculptSession &ss = *source.runtime->sculpt_session;
  const SubdivCCG &ccg = *ss.subdiv_ccg;
  const Mesh &base = *id_cast<const Mesh *>(source.data);
  if (ss.cache || ss.filter_cache || source.id.lib || base.id.lib || base.id.us != 1 ||
      !(U.uiflag & USER_GLOBALUNDO) || U.undosteps <= 0) {
    BKE_report(op->reports, RPT_ERROR, "Use a local single-user mesh and Global Undo; finish the stroke first");
    return OPERATOR_CANCELLED;
  }
  /* Snapshot/exit must not implicitly migrate uncommitted geometric layer data. */
  if (ccg.dirty.coords && !ss.multires_modifier->sculpt_layers.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "Commit pending sculpt geometry before HR extraction");
    return OPERATOR_CANCELLED;
  }
  if (ccg.dirty.hidden) {
    BKE_report(op->reports, RPT_ERROR,
               "Pending visibility must be flushed before HR extraction; V05 will not recompose geometric layers");
    return OPERATOR_CANCELLED;
  }
  const CCGKey key = BKE_subdiv_ccg_key_top_level(ccg);
  const int64_t samples = int64_t(ccg.grids_num) * key.grid_area;
  if (key.grid_size < 2 || samples <= 0 || samples > INT_MAX ||
      ccg.positions.size() != samples || ccg.grids_num != base.corners_num ||
      ccg.grid_to_face_map.size() != ccg.grids_num ||
      (!ccg.masks.is_empty() && ccg.masks.size() != samples) ||
      (!ccg.grid_hidden.is_empty() && (ccg.grid_hidden.size() != ccg.grids_num ||
       ccg.grid_hidden.group_size() != key.grid_area)) ||
      (by_face_set && (!hr_tools_valid(source) || selected_id <= 0))) {
    BKE_report(op->reports, RPT_ERROR, "Invalid evaluated Multires extraction data");
    return OPERATOR_CANCELLED;
  }
  if (!by_face_set && ccg.masks.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "Paint a sculpt mask first");
    return OPERATOR_CANCELLED;
  }
  const VArray<int> base_ids = *base.attributes().lookup_or_default<int>(
      ".sculpt_face_set", bke::AttrDomain::Face, 1);
  const auto *source_mmd=rc10_tools_enabled() ? rc2_source_master(source) : ss.multires_modifier;
  if (!source_mmd) {
    BKE_report(op->reports,RPT_ERROR,"HR extraction requires one active Multires modifier");
    return OPERATOR_CANCELLED;
  }
  const MultiresModifierData &mmd = *source_mmd;
  const float4x4 world = source.object_to_world();
  const float determinant = determinant_m4(world.ptr());
  if (!std::isfinite(determinant) || determinant == 0.0f) {
    BKE_report(op->reports, RPT_ERROR, "Source transform is singular or non-finite");
    return OPERATOR_CANCELLED;
  }
  const auto face_set_at = [&](const int index) {
    const int grid = index / key.grid_area, local = index % key.grid_area;
    const int fallback = base_ids[ccg.grid_to_face_map[grid]];
    return mmd.sculpt_face_set_grids ? bke::multires_layers::faceset_sample(mmd, grid,
        float(local % key.grid_size) / float(key.grid_size - 1),
        float(local / key.grid_size) / float(key.grid_size - 1), fallback) : fallback;
  };
  SubdivCCGNeighbors neighbors;
  HRExtractBuffersV05 extracted;
  const char *error = hr_extract_build_v05(
      ccg.grids_num, key.grid_size, by_face_set, selected_id,
      threshold, max_vertices,
      max_faces, determinant < 0.0f, face_set_at,
      [&](const int i) { return ccg.masks.is_empty() ? 0.0f : ccg.masks[i]; },
      [&](const int i) { return !ccg.grid_hidden.is_empty() &&
                              ccg.grid_hidden[i / key.grid_area][i % key.grid_area]; },
      [&](const int i) { return hr_extract_canonical_v05(ccg, key, i, neighbors); },
      [&](const int i) { return math::transform_point(world, ccg.positions[i]); }, extracted);
  if (error) {
    BKE_report(op->reports, RPT_ERROR, error);
    return OPERATOR_CANCELLED;
  }
  const int vertices_num = int(extracted.positions.size());
  const int faces_num = int(extracted.quads.size());
  Mesh *mesh = BKE_mesh_new_nomain(vertices_num, 0, faces_num, faces_num * 4);
  mesh->vert_positions_for_write().copy_from(extracted.positions.as_span());
  MutableSpan<int> offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corners = mesh->corner_verts_for_write();
  for (const int face : IndexRange(faces_num)) {
    offsets[face] = face * 4;
    for (const int corner : IndexRange(4)) {
      corners[face * 4 + corner] = extracted.quads[face][corner];
    }
  }
  offsets[faces_num] = faces_num * 4;
  bke::mesh_calc_edges(*mesh, false, false);
  auto ids = mesh->attributes_for_write().lookup_or_add_for_write_span<int>(
      ".sculpt_face_set", bke::AttrDomain::Face);
  ids.span.copy_from(extracted.face_sets.as_span());
  ids.finish();
  mesh->face_sets_color_default = base.face_sets_color_default;
  mesh->face_sets_color_seed = base.face_sets_color_seed;
  /* Same pre-extraction Undo/mode transition as Blender's native Mask Extract.
   * Output is staged before this point; caps/empty input never change the scene. */
  ED_undo_push_op(C, op);
  if (!object_sculpt_mode_exit(C, depsgraph)) {
    BKE_id_free(CTX_data_main(C), mesh);
    BKE_report(op->reports, RPT_ERROR, "Could not flush Sculpt state; no object created");
    return OPERATOR_CANCELLED;
  }
  const View3D *v3d = CTX_wm_view3d(C);
  const ushort local_bits = v3d && v3d->localvd ? v3d->local_view_uid : 0;
  const float zero[3] = {0, 0, 0};
  Object *created = ed::object::add_type(C, OB_MESH,
      by_face_set ? "HR Face Set Extract" : "HR Mask Extract", zero, zero, false, local_bits);
  BKE_mesh_nomain_to_mesh(mesh, id_cast<Mesh *>(created->data), created);
  if (add_solidify) {
    ModifierData *modifier = ed::object::modifier_add(op->reports, CTX_data_main(C),
        CTX_data_scene(C), created, "HR Extract Thickness", eModifierType_Solidify);
    if (modifier) {
      auto &solidify = *reinterpret_cast<SolidifyModifierData *>(modifier);
      solidify.offset = thickness;
      solidify.offset_fac = -1.0f;
    }
  }
  DEG_relations_tag_update(CTX_data_main(C));
  DEG_id_tag_update(&created->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, created);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, created->data);
  BKE_reportf(op->reports, RPT_INFO, "Created HR extraction: %d vertices, %d quads", vertices_num, faces_num);
  std::printf("[HR_EXTRACT_V05] vertices=%d faces=%d source=%s; new_object=1\n",
              vertices_num, faces_num, by_face_set ? "FACE_SET" : "MASK");
  std::fflush(stdout);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus hr_extract_exec_v05(bContext *C,wmOperator *op)
{
  return hr_extract_execute_v05(C,op,RNA_enum_get(op->ptr,"source")==1,
      RNA_int_get(op->ptr,"active_face_set"),RNA_float_get(op->ptr,"mask_threshold"),
      RNA_int_get(op->ptr,"max_vertices"),RNA_int_get(op->ptr,"max_faces"),
      RNA_boolean_get(op->ptr,"add_solidify"),RNA_float_get(op->ptr,"thickness"));
}
bool rc10_extract_requested(const Object &object)
{
  const auto *mmd=bke::facesets_visibility_rc2::modifier(object);
  return rc10_tools_enabled() && mmd && mmd->sculpt_face_set_grids;
}
wmOperatorStatus rc10_extract_from_native(bContext *C,wmOperator *op,const wmEvent *event)
{
  auto &object=*CTX_data_active_object(C);
  auto &depsgraph=*CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(&depsgraph,&object,false);
  if (!hr_tools_valid(object)) {
    BKE_report(op->reports,RPT_ERROR,"HR Extract requires Sculpt Level 1 or higher; no base-face fallback was applied");
    return OPERATOR_CANCELLED;
  }
  const float mouse[2]={float(event->mval[0]),float(event->mval[1])};
  vert_random_access_ensure(object);
  if (!cursor_geometry_info_update(C,mouse,false)) { return OPERATOR_CANCELLED; }
  const int id=hr_id_at_cursor(object);
  if (id<=0) { return OPERATOR_CANCELLED; }
  return hr_extract_execute_v05(C,op,true,id,.5f,250000,250000,
      RNA_boolean_get(op->ptr,"add_solidify"),.01f);
}

static wmOperatorStatus hr_extract_invoke_v05(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (RNA_enum_get(op->ptr, "source") == 1) {
    Object &source = *CTX_data_active_object(C);
    if (!hr_tools_valid(source)) {
      BKE_report(op->reports, RPT_ERROR, "A valid HR Face Set master is required");
      return OPERATOR_CANCELLED;
    }
    const float mouse[2] = {float(event->mval[0]), float(event->mval[1])};
    vert_random_access_ensure(source);
    cursor_geometry_info_update(C, mouse, false);
    const int id = hr_id_at_cursor(source);
    if (id <= 0) {
      BKE_report(op->reports, RPT_ERROR, "Place the cursor over a visible HR Face Set");
      return OPERATOR_CANCELLED;
    }
    RNA_int_set(op->ptr, "active_face_set", id);
  }
  return WM_operator_props_dialog_popup(C, op, 420);
}

void SCULPT_OT_multires_hr_extract_v05(wmOperatorType *ot)
{
  ot->name = "Extract Multires HR Region";
  ot->idname = "SCULPT_OT_multires_hr_extract_v05";
  ot->description = "Create a separate mesh from current Multires mask or HR Face Set; preserve source geometry";
  ot->poll = hr_extract_poll_v05;
  ot->exec = hr_extract_exec_v05;
  ot->invoke = hr_extract_invoke_v05;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  static const EnumPropertyItem sources[] = {
      {0, "MASK", 0, "Sculpt Mask", "Extract quads whose four visible corners meet the mask threshold"},
      {1, "FACE_SET", 0, "HR Face Set", "Extract quads whose four visible corners belong to this group"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_enum(ot->srna, "source", sources, 0, "Source", "Extraction source");
  RNA_def_int(ot->srna, "active_face_set", 1, 1, INT_MAX, "Face Set ID", "Group selected on the surface", 1, INT_MAX);
  RNA_def_float(ot->srna, "mask_threshold", 0.5f, 0, 1, "Mask Threshold", "All four corners must meet this value", 0, 1);
  RNA_def_int(ot->srna, "max_vertices", 250000, 4, 4000000, "Vertex Limit", "Reject before creating an oversized output", 4, 1000000);
  RNA_def_int(ot->srna, "max_faces", 250000, 1, 4000000, "Quad Limit", "Reject before creating an oversized output", 1, 1000000);
  RNA_def_boolean(ot->srna, "add_solidify", false, "Add Thickness", "Add an editable Solidify modifier to the new object");
  RNA_def_float(ot->srna, "thickness", 0.01f, 0, 1000, "Thickness", "World-space thickness on the new identity-transform object", 0, 1);
}
