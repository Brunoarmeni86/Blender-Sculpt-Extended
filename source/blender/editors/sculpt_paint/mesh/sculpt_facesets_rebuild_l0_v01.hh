/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included inside blender::ed::sculpt_paint. Object-mode data probe only. */
namespace facesets_rebuild_l0_v01 {
struct MasterPlan {
  MultiresFaceSetGrid *grids = nullptr;
  int count = 0, level = 0, first = 0, selected_count = 0;
  ~MasterPlan() { if (grids) { MEM_delete(grids); } }
  MasterPlan() = default;
  MasterPlan(const MasterPlan &) = delete;
  MasterPlan &operator=(const MasterPlan &) = delete;
};
/* Preparation owns only the new descriptor array. Non-target dense payloads
 * remain borrowed until publish; failure must never free live payloads. */
inline const char *stage_master(const MultiresModifierData &mmd,
                                const int grids_num, const int first, const int count,
                                const int id, const Span<int> initial_ids, MasterPlan &plan)
{
  if (plan.grids || grids_num <= 0 || first < 0 || count <= 0 ||
      first > grids_num || count > grids_num-first || id <= 0 ||
      mmd.totlvl < 1 || mmd.totlvl > 13)
  { return "Invalid face/grid range, ID or Multires level"; }
  const bool present=mmd.sculpt_face_set_grids!=nullptr;
  if (!present && (mmd.sculpt_face_set_grids_num!=0 || mmd.sculpt_face_set_level!=0))
  { return "Inconsistent absent Face Set master"; }
  if (present && (mmd.sculpt_face_set_grids_num!=grids_num ||
                 mmd.sculpt_face_set_level!=mmd.totlvl))
  { return "Master layout/maximum level differs; automatic conversion is disabled in this probe"; }
  if (!present && initial_ids.size()!=grids_num)
  { return "Missing base IDs for master initialization"; }
  const int level=present?mmd.sculpt_face_set_level:int(mmd.totlvl);
  const int side=(1<<(level-1))+1,area=side*side;
  if (present) {
    for (int i=0;i<grids_num;i++) {
      const MultiresFaceSetGrid &grid=mmd.sculpt_face_set_grids[i];
      if (grid.level!=level || (grid.ids?grid.totid!=area:grid.totid!=0))
      { return "Invalid stored Face Set grid; no assignment applied"; }
    }
  }
  plan.grids=MEM_new_array<MultiresFaceSetGrid>(grids_num,"FaceSets rebuild staged descriptors");
  plan.count=grids_num;plan.level=level;plan.first=first;plan.selected_count=count;
  for (int i=0;i<grids_num;i++) {
    if (present) { plan.grids[i]=mmd.sculpt_face_set_grids[i]; }
    else { plan.grids[i].level=level;plan.grids[i].uniform_id=initial_ids[i]; }
  }
  for (int i=first;i<first+count;i++) {
    plan.grids[i].ids=nullptr;plan.grids[i].totid=0;plan.grids[i].uniform_id=id;
  }
  return nullptr;
}
/* No allocations, validation or callbacks during publication. */
inline void publish_master(MultiresModifierData &mmd, MasterPlan &plan)
{
  MultiresFaceSetGrid *previous=mmd.sculpt_face_set_grids;
  mmd.sculpt_face_set_grids=plan.grids;
  mmd.sculpt_face_set_grids_num=plan.count;
  mmd.sculpt_face_set_level=plan.level;
  plan.grids=nullptr;
  if (previous) {
    for (int i=plan.first;i<plan.first+plan.selected_count;i++) { if (previous[i].ids) { MEM_delete(previous[i].ids); } }
    MEM_delete(previous);
  }
}
/* END_STANDALONE_CORE */
inline bool enabled()
{
  const char *value=BLI_getenv("BLENDER_FACESETS_REBUILD_L0_V01");
  return value && value[0]=='1' && value[1]=='\0';
}
inline MultiresModifierData *original_modifier(Object &ob)
{
  return reinterpret_cast<MultiresModifierData *>(BKE_modifiers_findby_type(&ob,eModifierType_Multires));
}
inline const char *binding_error(const MultiresModifierData &mmd,const Mesh &mesh)
{
  if (mmd.sculpt_corner_ids_num<0 ||
      bool(mmd.sculpt_corner_ids)!=(mmd.sculpt_corner_ids_num>0))
  { return "Invalid corner binding"; }
  if (mmd.sculpt_corner_ids) {
    const VArraySpan<int> ids=*mesh.attributes().lookup<int>(
        bke::multires_layers::corner_id_attribute,bke::AttrDomain::Corner);
    if (mmd.sculpt_corner_ids_num!=mesh.corners_num || ids.size()!=mesh.corners_num)
    { return "Missing/mismatched corner identities"; }
    for (int i=0;i<mesh.corners_num;i++) {
      if (mmd.sculpt_corner_ids[i]!=ids[i])
      { return "Corner storage is not in canonical order; remapping is outside this probe"; }
    }
  }
  return nullptr;
}
inline bool poll(bContext *C)
{
  const Object *ob=CTX_data_active_object(C);
  return enabled() && ob && ob->type==OB_MESH && ob->mode==OB_MODE_OBJECT &&
      BKE_modifiers_findby_type(ob,eModifierType_Multires);
}
inline wmOperatorStatus assign_exec(bContext *C,wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  MultiresModifierData &mmd=*original_modifier(ob);
  UndoStack *stack=ED_undo_stack_get();
  if (ob.id.lib || ob.id.override_library || mesh.id.lib || mesh.id.override_library ||
      mesh.id.us!=1 || mesh.key || ob.modifiers.count()!=1 || ob.runtime->sculpt_session ||
      !(U.uiflag&USER_GLOBALUNDO) || U.undosteps<=0 || !stack || stack->step_init ||
      stack->group_level!=0 || mmd.lvl!=0 || mmd.sculptlvl!=0)
  {
    BKE_report(op->reports,RPT_ERROR,
        "Probe requires local single-user Object Mode, one Multires, both viewport/sculpt levels 0, Global Undo and no Sculpt session");
    return OPERATOR_CANCELLED;
  }
  if (const char *error=binding_error(mmd,mesh)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  const int face=RNA_int_get(op->ptr,"face_index"),id=RNA_int_get(op->ptr,"face_set_id");
  if (face<0 || face>=mesh.faces_num || id<=0) {
    BKE_report(op->reports,RPT_ERROR,"Face index or ID out of range");return OPERATOR_CANCELLED;
  }
  const IndexRange corners=mesh.faces()[face];
  const VArraySpan<int> before_ids=*mesh.attributes().lookup_or_default<int>(
      ".sculpt_face_set",bke::AttrDomain::Face,1);
  Array<int> base_before(mesh.faces_num);
  for (const int f:mesh.faces().index_range()) { base_before[f]=before_ids[f]; }
  Array<int> initial;
  if (!mmd.sculpt_face_set_grids) {
    initial.reinitialize(mesh.corners_num);
    for (const int f:mesh.faces().index_range()) {
      for (const int corner:mesh.faces()[f]) { initial[corner]=base_before[f]; }
    }
  }
  MasterPlan plan;
  if (const char *error=stage_master(mmd,mesh.corners_num,int(corners.start()),
                                    int(corners.size()),id,initial.as_span(),plan)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  /* A preceding Sculpt Undo step is not a complete Object Mode checkpoint.
   * Capture the current original object+modifier before publishing any IDs. */
  const UndoStep *previous=stack->step_active;
  ED_undo_push(C,"Before FaceSets L0 Probe");
  stack=ED_undo_stack_get();
  if (!stack || stack->step_init || !stack->step_active || stack->step_active==previous ||
      stack->step_active->type!=BKE_UNDOSYS_TYPE_MEMFILE) {
    BKE_report(op->reports,RPT_ERROR,"Global pre-assignment checkpoint failed; IDs unchanged");
    return OPERATOR_CANCELLED;
  }
  auto writer=mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
      ".sculpt_face_set",bke::AttrDomain::Face);
  if (!writer) {
    BKE_report(op->reports,RPT_ERROR,"Cannot acquire base Face Set attribute");
    return OPERATOR_CANCELLED;
  }
  /* The new attribute must initialize untouched faces to the same IDs used
   * for the master (normally 1); attribute allocation defaults to zero. */
  for (const int f:mesh.faces().index_range()) { writer.span[f]=base_before[f]; }
  writer.span[face]=id;
  writer.finish();
  publish_master(mmd,plan);
  DEG_id_tag_update(&mesh.id,ID_RECALC_GEOMETRY);
  DEG_id_tag_update(&ob.id,ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_OBJECT|ND_MODIFIER,&ob);
  WM_event_add_notifier(C,NC_OBJECT|ND_DRAW,&ob);
  std::printf("[FACESETS_REBUILD_L0_V01] assigned face=%d id=%d corners=%lld master_level=%d dense_created=0\n",
      face,id,static_cast<long long>(corners.size()),mmd.sculpt_face_set_level);
  std::fflush(stdout);
  return OPERATOR_FINISHED;
}
inline bool audit_poll(bContext *C)
{
  const Object *ob=CTX_data_active_object(C);
  return enabled() && ob && ob->type==OB_MESH && ELEM(ob->mode,OB_MODE_OBJECT,OB_MODE_SCULPT) &&
      BKE_modifiers_findby_type(ob,eModifierType_Multires);
}
inline wmOperatorStatus audit_exec(bContext *C,wmOperator *op)
{
  const Object &ob=*CTX_data_active_object(C);
  if (ob.runtime->sculpt_session && (ob.runtime->sculpt_session->cache || ob.runtime->sculpt_session->filter_cache)) {
    BKE_report(op->reports,RPT_ERROR,"Finish the stroke before reading IDs");return OPERATOR_CANCELLED;
  }
  const Mesh &mesh=*id_cast<const Mesh *>(ob.data);
  const MultiresModifierData &mmd=*original_modifier(*const_cast<Object *>(&ob));
  const int face=RNA_int_get(op->ptr,"face_index"),expected=RNA_int_get(op->ptr,"expected_id");
  const int expected_present=RNA_int_get(op->ptr,"expect_master");
  if (face<0 || face>=mesh.faces_num) {
    BKE_report(op->reports,RPT_ERROR,"Face index out of range");return OPERATOR_CANCELLED;
  }
  if (const char *error=binding_error(mmd,mesh)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  const bool present=mmd.sculpt_face_set_grids!=nullptr;
  if ((expected_present>=0 && int(present)!=expected_present) ||
      (present && !bke::multires_layers::faceset_master_valid(mmd,mesh.corners_num)) ||
      (!present && (mmd.sculpt_face_set_grids_num!=0 || mmd.sculpt_face_set_level!=0)))
  { BKE_report(op->reports,RPT_ERROR,"Unexpected/invalid Face Set master");return OPERATOR_CANCELLED; }
  const VArraySpan<int> base=*mesh.attributes().lookup_or_default<int>(
      ".sculpt_face_set",bke::AttrDomain::Face,1);
  uint64_t mismatch=0,dense=0;
  if (expected>0) { mismatch+=base[face]!=expected; }
  for (const int corner:mesh.faces()[face]) {
    if (!present) { continue; }
    const MultiresFaceSetGrid &grid=mmd.sculpt_face_set_grids[corner];
    dense+=grid.ids!=nullptr;
    if (expected>0) {
      if (grid.ids) { for (int i=0;i<grid.totid;i++) { mismatch+=grid.ids[i]!=expected; } }
      else { mismatch+=grid.uniform_id!=expected; }
    }
  }
  std::printf("[FACESETS_REBUILD_L0_V01_AUDIT] face=%d base_id=%d present=%d master_level=%d viewport_level=%d sculpt_level=%d selected_dense=%llu expected=%d mismatches=%llu\n",
      face,base[face],int(present),mmd.sculpt_face_set_level,int(mmd.lvl),int(mmd.sculptlvl),
      static_cast<unsigned long long>(dense),expected,static_cast<unsigned long long>(mismatch));
  std::fflush(stdout);
  if (mismatch) { BKE_report(op->reports,RPT_ERROR,"Face Set descendant IDs differ from expectation");return OPERATOR_CANCELLED; }
  BKE_report(op->reports,RPT_INFO,"Face Set data verified; no evaluation, tags or Undo push");
  return OPERATOR_FINISHED;
}
}  // namespace facesets_rebuild_l0_v01
static void OBJECT_OT_facesets_rebuild_l0_assign_v01(wmOperatorType *ot)
{
  ot->name="FaceSets Rebuild Assign L0 V01";
  ot->idname="OBJECT_OT_facesets_rebuild_l0_assign_v01";
  ot->description="Data-only probe: assign one base face and all its stored descendants in one global Undo operation";
  ot->poll=facesets_rebuild_l0_v01::poll;ot->exec=facesets_rebuild_l0_v01::assign_exec;
  ot->flag=OPTYPE_REGISTER|OPTYPE_UNDO;
  RNA_def_int(ot->srna,"face_index",0,0,INT_MAX,"Base Face","Index of the original mesh face",0,100);
  RNA_def_int(ot->srna,"face_set_id",7,1,INT_MAX,"Face Set ID","Categorical ID",1,100);
}
static void OBJECT_OT_facesets_rebuild_l0_audit_v01(wmOperatorType *ot)
{
  ot->name="FaceSets Rebuild Audit L0 V01";
  ot->idname="OBJECT_OT_facesets_rebuild_l0_audit_v01";
  ot->description="Read stored descendant IDs without evaluation or data mutation";
  ot->poll=facesets_rebuild_l0_v01::audit_poll;ot->exec=facesets_rebuild_l0_v01::audit_exec;
  RNA_def_int(ot->srna,"face_index",0,0,INT_MAX,"Base Face","Face to verify",0,100);
  RNA_def_int(ot->srna,"expected_id",0,0,INT_MAX,"Expected ID","Zero disables ID comparison",0,100);
  RNA_def_int(ot->srna,"expect_master",-1,-1,1,"Expected Master","-1 any, 0 absent, 1 present",-1,1);
}
