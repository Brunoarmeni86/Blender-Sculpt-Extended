/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diagnostics for a generated small cube. Included in sculpt namespace. */
namespace facesets_brush_probe_v05 {
inline bool enabled()
{
  const char *v=BLI_getenv("BLENDER_FACESETS_BRUSH_V05");
  return v && v[0]=='1' && v[1]=='\0';
}
inline bool dense_shape_poll(bContext *C)
{
  Object *ob=CTX_data_active_object(C);
  if (!enabled() || !ob || ob->type!=OB_MESH || !ob->data) { return false; }
  const auto *mmd=facesets_rebuild_l0_v01::original_modifier(*ob);
  return mmd && bke::facesets_dense_v06d::shape_valid(*ob,*id_cast<Mesh *>(ob->data),*mmd);
}
inline bool poll(bContext *C)
{
  return enabled() && (dense_shape_poll(C) || facesets_mixed_fixture_v03::poll(C));
}
inline const char *fixture_error(Object &ob,Mesh &mesh,
                                 MultiresModifierData &mmd)
{
  if (bke::facesets_dense_v06d::shape_valid(ob,mesh,mmd)) { return nullptr; }
  return facesets_mixed_fixture_v03::fixture_error(ob,mesh,mmd);
}
inline wmOperatorStatus activate_exec(bContext *C,wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  auto &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  if (const char *error=fixture_error(ob,mesh,mmd)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  if (bke::facesets_rc1::eligible(ob,mesh,mmd)) {
    auto base_ids=face_set::ensure_face_sets_mesh(mesh);base_ids.finish();
    if (!bke::multires_layers::faceset_ensure_from_mesh(mmd,mesh,mmd.totlvl) ||
        !bke::facesets_rc1::master_valid(ob,mesh,mmd)) {
      BKE_report(op->reports,RPT_ERROR,"Cannot initialize Face Sets master");return OPERATOR_CANCELLED;
    }
  }
  if (ob.mode!=OB_MODE_SCULPT || !bke::multires_layers::faceset_master_valid(mmd,mesh.corners_num)) {
    BKE_report(op->reports,RPT_ERROR,"Enter Sculpt on the generated cube with prepared master");return OPERATOR_CANCELLED;
  }
  Brush *brush=BKE_brush_add(CTX_data_main(C),"Face Sets Rebuild V05",OB_MODE_SCULPT);
  brush->sculpt_brush_type=SCULPT_BRUSH_TYPE_DRAW_FACE_SETS;
  brush->stroke_method=BRUSH_STROKE_DOTS;
  brush->alpha=1.0f;brush->size=70;
  Paint *paint=BKE_paint_get_active_from_context(C);
  if (!paint || !WM_toolsystem_activate_brush_and_tool(C,paint,brush)) {
    BKE_report(op->reports,RPT_ERROR,"Cannot activate local Face Sets brush in VIEW_3D");return OPERATOR_CANCELLED;
  }
  WM_main_add_notifier(NC_ASSET|NA_ACTIVATED,nullptr);
  WM_main_add_notifier(NC_SCENE|ND_TOOLSETTINGS,nullptr);
  return OPERATOR_FINISHED;
}
inline wmOperatorStatus export_exec(bContext *C,wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  auto &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  if (const char *error=fixture_error(ob,mesh,mmd)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  if (!bke::multires_layers::faceset_master_valid(mmd,mesh.corners_num)) { return OPERATOR_CANCELLED; }
  const VArraySpan<int> base=*mesh.attributes().lookup_or_default<int>(".sculpt_face_set",bke::AttrDomain::Face,0);
  char path[FILE_MAX];RNA_string_get(op->ptr,"filepath",path);
  FILE *out=std::fopen(path,"wb");
  if (!out) { BKE_report(op->reports,RPT_ERROR,"Cannot write diagnostic JSON");return OPERATOR_CANCELLED; }
  std::fprintf(out,"{\"master_level\":%d,\"base\":[",mmd.sculpt_face_set_level);
  for (int f=0;f<mesh.faces_num;f++) { std::fprintf(out,"%s%d",f?",":"",base[f]); }
  std::fprintf(out,"],\"grids\":[");
  const int side=bke::subdiv::grid_size_from_level(mmd.sculpt_face_set_level);
  for (int c=0;c<mesh.corners_num;c++) {
    const auto &grid=mmd.sculpt_face_set_grids[c];
    std::fprintf(out,"%s[",c?",":"");
    for (int i=0;i<side*side;i++) { std::fprintf(out,"%s%d",i?",":"",grid.ids?grid.ids[i]:grid.uniform_id); }
    std::fprintf(out,"]");
  }
  const char *vis_flag=BLI_getenv("BLENDER_FACESETS_VISIBILITY_RC2");
  if (vis_flag && vis_flag[0]=='1' && vis_flag[1]=='\0') {
    std::fprintf(out,"],\"hidden\":[");
    const SubdivCCG *ccg=ob.runtime->sculpt_session?ob.runtime->sculpt_session->subdiv_ccg:nullptr;
    bool first=true;
    if (ccg && !ccg->grid_hidden.is_empty()) {
      for (const int grid:IndexRange(ccg->grids_num)) {
        for (const int i:IndexRange(ccg->grid_area)) {
          if (ccg->grid_hidden[grid][i]) {
            std::fprintf(out,"%s%d",first?"":",",grid*ccg->grid_area+i);first=false;
          }
        }
      }
    }
    std::fprintf(out,"],\"hidden_faces\":[");
    const auto hide_faces=*mesh.attributes().lookup_or_default<bool>(".hide_poly",bke::AttrDomain::Face,false);
    bool first_face=true;
    for (const int f:mesh.faces().index_range()) {
      if (hide_faces[f]) { std::fprintf(out,"%s%d",first_face?"":",",f);first_face=false; }
    }
    std::fprintf(out,"]}\n");
  }
  else { std::fprintf(out,"]}\n"); }
  const bool failed=std::ferror(out)!=0;
  const int close_status=std::fclose(out);
  if (failed || close_status) { BKE_report(op->reports,RPT_ERROR,"Diagnostic write failed");return OPERATOR_CANCELLED; }
  std::printf("[FACESETS_BRUSH_V05_EXPORT] %d IDs exported; level=%d mode=%d\n",mesh.corners_num*side*side,int(mmd.sculptlvl),int(ob.mode));
  std::fflush(stdout);
  return OPERATOR_FINISHED;
}

inline wmOperatorStatus dense_prepare_exec(bContext *C,wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  auto &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  if (ob.mode!=OB_MODE_OBJECT || mmd.sculpt_face_set_grids ||
      !bke::multires_layers::faceset_ensure_from_mesh(mmd,mesh,mmd.totlvl) ||
      !bke::multires_layers::faceset_master_valid(mmd,mesh.corners_num)) {
    BKE_report(op->reports,RPT_ERROR,"Prepare requires a new eligible mesh trial in Object Mode");
    return OPERATOR_CANCELLED;
  }
  DEG_id_tag_update(&mesh.id,ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C,NC_OBJECT|ND_DRAW,&ob);
  return OPERATOR_FINISHED;
}
/* Object L0 diagnostic only. Full native Object Undo captures the whole master;
 * this does not claim validation of the separate Sculpt L0 snapshot path. */
inline wmOperatorStatus dense_l0_assign_exec(bContext *C,wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  auto &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  const int face=RNA_int_get(op->ptr,"face_index");
  const int id=RNA_int_get(op->ptr,"face_set_id");
  if (ob.mode!=OB_MODE_OBJECT || mmd.sculptlvl!=0 || mmd.lvl!=0 ||
      face<0 || face>=mesh.faces_num || id<1 ||
      !bke::multires_layers::faceset_master_valid(mmd,mesh.corners_num)) {
    BKE_report(op->reports,RPT_ERROR,"Assign requires a valid dense fixture in Object L0");
    return OPERATOR_CANCELLED;
  }
  auto base=face_set::ensure_face_sets_mesh(mesh);
  base.span[face]=id;
  for (const int corner:mesh.faces()[face]) {
    auto &grid=mmd.sculpt_face_set_grids[corner];
    MEM_SAFE_DELETE(grid.ids);
    grid.totid=0;grid.level=mmd.sculpt_face_set_level;grid.uniform_id=id;
  }
  base.finish();
  DEG_id_tag_update(&mesh.id,ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C,NC_OBJECT|ND_DRAW,&ob);
  return OPERATOR_FINISHED;
}
} // namespace facesets_brush_probe_v05
static void SCULPT_OT_facesets_brush_activate_v05(wmOperatorType *ot)
{
  ot->name="Activate Face Sets Rebuild V05";ot->idname="SCULPT_OT_facesets_brush_activate_v05";
  ot->description="Activate a local native Face Sets brush on the generated test cube";
  ot->poll=facesets_brush_probe_v05::poll;ot->exec=facesets_brush_probe_v05::activate_exec;ot->flag=OPTYPE_INTERNAL;
}
static void OBJECT_OT_facesets_brush_export_v05(wmOperatorType *ot)
{
  ot->name="Face Sets Brush Data Export V05";ot->idname="OBJECT_OT_facesets_brush_export_v05";
  ot->description="Read all test IDs into a JSON file without changing the master";
  ot->poll=facesets_brush_probe_v05::poll;ot->exec=facesets_brush_probe_v05::export_exec;ot->flag=OPTYPE_INTERNAL;
  RNA_def_string(ot->srna,"filepath",nullptr,FILE_MAX,"File","Diagnostic JSON output path");
}

static void OBJECT_OT_facesets_dense_prepare_v06d(wmOperatorType *ot)
{
  ot->name="Prepare Dense Face Sets V06d";ot->idname="OBJECT_OT_facesets_dense_prepare_v06d";
  ot->description="Initialize the independent master on the bounded generated dense fixture";
  ot->poll=facesets_brush_probe_v05::dense_shape_poll;
  ot->exec=facesets_brush_probe_v05::dense_prepare_exec;ot->flag=OPTYPE_INTERNAL;
}
static void OBJECT_OT_facesets_dense_l0_assign_v06d(wmOperatorType *ot)
{
  ot->name="Assign Dense L0 Face Set V06d";ot->idname="OBJECT_OT_facesets_dense_l0_assign_v06d";
  ot->description="Diagnostic Object L0 assignment with native full Undo on the generated dense fixture";
  ot->poll=facesets_brush_probe_v05::dense_shape_poll;
  ot->exec=facesets_brush_probe_v05::dense_l0_assign_exec;
  ot->flag=OPTYPE_INTERNAL|OPTYPE_UNDO;
  RNA_def_int(ot->srna,"face_index",0,0,4095,"Face","Trial base face",0,4095);
  RNA_def_int(ot->srna,"face_set_id",7,1,1000000,"Face Set","Diagnostic ID",1,1000000);
}
