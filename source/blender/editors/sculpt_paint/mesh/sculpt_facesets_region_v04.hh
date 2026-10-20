/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diagnostic region writer; included after the V03 fixture. */
namespace facesets_region_v04 {
inline bool enabled()
{
  const char *v=BLI_getenv("BLENDER_FACESETS_REGION_V04");
  return v && v[0]=='1' && v[1]=='\0';
}
inline bool poll(bContext *C)
{
  return enabled() && facesets_sculpt_l0_v02::poll(C);
}
inline wmOperatorStatus assign_exec(bContext *C,wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  auto &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  SculptSession &ss=*ob.runtime->sculpt_session;
  if (const char *error=facesets_mixed_fixture_v03::fixture_error(ob,mesh,mmd)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  UndoStack *stack=ED_undo_stack_get();
  if (!(U.uiflag&USER_GLOBALUNDO) || U.undosteps<=0 || !stack || stack->step_init ||
      stack->group_level || mmd.lvl!=mmd.sculptlvl || mmd.sculptlvl<2 || mmd.sculptlvl>3 ||
      mmd.sculpt_displacement_enabled || mmd.sculpt_shrinkwrap_enabled ||
      !bke::multires_layers::faceset_master_valid(mmd,mesh.corners_num)) {
    BKE_report(op->reports,RPT_ERROR,"Region diagnostic requires existing master, levels 2 or 3, Global Undo, no live deformation");return OPERATOR_CANCELLED;
  }
  Depsgraph &depsgraph=*CTX_data_depsgraph_pointer(C);
  BKE_sculpt_update_object_for_edit(&depsgraph,&ob,false);
  auto *pbvh=bke::object::pbvh_get(ob);
  if (!pbvh || pbvh->type()!=bke::pbvh::Type::Grids || !ss.subdiv_ccg || ss.subdiv_ccg->level!=mmd.sculptlvl || ss.multires_modifier!=&mmd) {
    BKE_report(op->reports,RPT_ERROR,"Expected PBVH Grids and original Multires master");return OPERATOR_CANCELLED;
  }
  const int id=RNA_int_get(op->ptr,"face_set_id");
  const int side=bke::subdiv::grid_size_from_level(mmd.sculptlvl);
  const int x=RNA_int_get(op->ptr,"sample_x"),y=RNA_int_get(op->ptr,"sample_y");
  if (x>=side || y>=side || !mmd.sculpt_face_set_grids[0].ids || mmd.sculpt_face_set_grids[0].totid!=25) {
    BKE_report(op->reports,RPT_ERROR,"Sample outside grid or target is not the dense test grid");return OPERATOR_CANCELLED;
  }
  Array<float> factors(side*side,0.0f);factors[y*side+x]=1.0f;
  IndexMaskMemory memory;
  const IndexMask leaves=bke::pbvh::all_leaf_nodes(*pbvh,memory);
  const Span<bke::pbvh::GridsNode> nodes=pbvh->nodes<bke::pbvh::GridsNode>();
  const IndexMask affected=IndexMask::from_predicate(leaves,memory,[&](const int node) {
    for (const int grid:nodes[node].grids()) { if (grid==0) { return true; } }return false;
  },exec_mode::grain_size(1));
  if (affected.is_empty()) { BKE_report(op->reports,RPT_ERROR,"Target grid has no PBVH leaf");return OPERATOR_CANCELLED; }
  /* Existing native Grids FaceSet Undo captures the retained master before mutation.
   * Dense fixture avoids allocation in faceset_paint_grid after this checkpoint. */
  undo::push_begin(*CTX_data_scene(C),ob,op);
  undo::push_nodes(depsgraph,ob,affected,undo::Type::FaceSet);
  const bool changed=bke::multires_layers::faceset_paint_grid(mmd,0,side,factors.as_span(),0.5f,id);
  pbvh->tag_face_sets_changed(affected);
  undo::push_end(ob);
  WM_event_add_notifier(C,NC_OBJECT|ND_DRAW,&ob);
  std::printf("[FACESETS_REGION_V04] grid=0 level=%d sample=(%d,%d) id=%d changed=%d\n",int(mmd.sculptlvl),x,y,id,int(changed));
  std::fflush(stdout);
  return OPERATOR_FINISHED;
}
inline bool audit_poll(bContext *C)
{
  return enabled() && facesets_mixed_fixture_v03::poll(C);
}
inline wmOperatorStatus audit_exec(bContext *C,wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  auto &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  if (const char *error=facesets_mixed_fixture_v03::fixture_error(ob,mesh,mmd)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  if (!bke::multires_layers::faceset_master_valid(mmd,24) || mmd.sculpt_face_set_level!=3) {
    BKE_report(op->reports,RPT_ERROR,"Invalid retained test master");return OPERATOR_CANCELLED;
  }
  const int phase=RNA_int_get(op->ptr,"phase");
  const VArraySpan<int> base=*mesh.attributes().lookup_or_default<int>(".sculpt_face_set",bke::AttrDomain::Face,0);
  uint64_t mismatch=0,checked=0;
  for (int f=0;f<6;f++) { mismatch+=base[f]!=1; }
  for (int corner=0;corner<24;corner++) {
    const auto &grid=mmd.sculpt_face_set_grids[corner];
    if (!grid.ids || grid.totid!=25 || grid.level!=3 || grid.uniform_id!=1) { mismatch++;continue; }
    for (int y=0;y<5;y++) { for (int x=0;x<5;x++) {
      int expected=facesets_mixed_fixture_v03::sample_id(corner,y*5+x);
      /* Independent explicit oracle: level 2 central sample owns [1,2] x [1,2]. */
      if (phase>=1 && corner==0 && x>=1 && x<=2 && y>=1 && y<=2) { expected=7; }
      if (phase>=2 && corner==0 && x==3 && y==3) { expected=9; }
      mismatch+=grid.ids[y*5+x]!=expected;checked++;
    } }
  }
  std::printf("[FACESETS_REGION_V04_AUDIT] phase=%d samples=%llu mismatches=%llu level=%d mode=%d\n",phase,
      static_cast<unsigned long long>(checked),static_cast<unsigned long long>(mismatch),int(mmd.sculptlvl),int(ob.mode));
  std::fflush(stdout);
  if (mismatch) { BKE_report(op->reports,RPT_ERROR,"Region update differs from exact 600-ID oracle");return OPERATOR_CANCELLED; }
  return OPERATOR_FINISHED;
}
} // namespace facesets_region_v04
static void SCULPT_OT_facesets_region_assign_v04(wmOperatorType *ot)
{
  ot->name="FaceSets Region Assign V04";ot->idname="SCULPT_OT_facesets_region_assign_v04";
  ot->description="Diagnostic region write on generated test cube with native Grids Undo";
  ot->poll=facesets_region_v04::poll;ot->exec=facesets_region_v04::assign_exec;
  ot->flag=OPTYPE_INTERNAL|OPTYPE_UNDO;
  RNA_def_int(ot->srna,"face_set_id",7,1,INT_MAX,"ID","Categorical ID",1,100);
  RNA_def_int(ot->srna,"sample_x",1,0,4,"X","Live grid sample",0,4);
  RNA_def_int(ot->srna,"sample_y",1,0,4,"Y","Live grid sample",0,4);
}
static void OBJECT_OT_facesets_region_audit_v04(wmOperatorType *ot)
{
  ot->name="FaceSets Region Audit V04";ot->idname="OBJECT_OT_facesets_region_audit_v04";
  ot->description="Compare all retained IDs against the independent region fixture oracle";
  ot->poll=facesets_region_v04::audit_poll;ot->exec=facesets_region_v04::audit_exec;ot->flag=OPTYPE_INTERNAL;
  RNA_def_int(ot->srna,"phase",0,0,2,"Phase","0 original, 1 coarse write, 2 additional finest write",0,2);
}
