/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included inside blender::ed::sculpt_paint, after the Object-mode probe. */
namespace facesets_sculpt_l0_v02 {
inline bool enabled()
{
  const char *v=BLI_getenv("BLENDER_FACESETS_SCULPT_L0_V02");
  return v && v[0]=='1' && v[1]=='\0';
}
inline bool poll(bContext *C)
{
  const Object *ob=CTX_data_active_object(C);
  return enabled() && ob && ob->type==OB_MESH && ob->mode==OB_MODE_SCULPT &&
      ob->runtime->sculpt_session && BKE_modifiers_findby_type(ob,eModifierType_Multires);
}
inline wmOperatorStatus assign_exec(bContext *C,wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  MultiresModifierData &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  SculptSession &ss=*ob.runtime->sculpt_session;
  UndoStack *stack=ED_undo_stack_get();
  if (ob.id.lib || ob.id.override_library || mesh.id.lib || mesh.id.override_library ||
      mesh.id.us!=1 || mesh.key || ob.modifiers.count()!=1 || ss.cache || ss.filter_cache ||
      !(U.uiflag&USER_GLOBALUNDO) || U.undosteps<=0 || !stack || stack->step_init ||
      stack->group_level || mmd.sculptlvl!=0 || mmd.lvl!=0 || mmd.sculpt_displacement_enabled ||
      mmd.sculpt_shrinkwrap_enabled || !mesh.attributes().contains(".sculpt_face_set"))
  {
    BKE_report(op->reports,RPT_ERROR,
        "Sculpt L0 probe requires local single-user Multires, both levels 0, existing base Face Sets, no active stroke/live deformation and Global Undo");
    return OPERATOR_CANCELLED;
  }
  if (const char *error=facesets_rebuild_l0_v01::binding_error(mmd,mesh)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  Depsgraph &depsgraph=*CTX_data_depsgraph_pointer(C);
  BKE_sculpt_update_object_for_edit(&depsgraph,&ob,false);
  bke::pbvh::Tree *pbvh=bke::object::pbvh_get(ob);
  if (!pbvh || pbvh->type()!=bke::pbvh::Type::Mesh || ss.subdiv_ccg) {
    BKE_report(op->reports,RPT_ERROR,"Probe requires PBVH Mesh at Sculpt Level 0");return OPERATOR_CANCELLED;
  }
  const int face=RNA_int_get(op->ptr,"face_index"),id=RNA_int_get(op->ptr,"face_set_id");
  if (face<0 || face>=mesh.faces_num || id<=0) {
    BKE_report(op->reports,RPT_ERROR,"Face index or ID out of range");return OPERATOR_CANCELLED;
  }
  const IndexRange corners=mesh.faces()[face];
  const VArraySpan<int> base_ids=*mesh.attributes().lookup<int>(".sculpt_face_set",bke::AttrDomain::Face);
  if (base_ids.size()!=mesh.faces_num) {
    BKE_report(op->reports,RPT_ERROR,"Invalid base Face Set attribute");return OPERATOR_CANCELLED;
  }
  Array<int> initial;
  if (!mmd.sculpt_face_set_grids) {
    initial.reinitialize(mesh.corners_num);
    for (const int f:mesh.faces().index_range()) { for (const int corner:mesh.faces()[f]) { initial[corner]=base_ids[f]; } }
  }
  facesets_rebuild_l0_v01::MasterPlan plan;
  if (const char *error=facesets_rebuild_l0_v01::stage_master(
          mmd,mesh.corners_num,int(corners.start()),int(corners.size()),id,initial.as_span(),plan)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  Array<int> indices(corners.size());
  for (const int i:indices.index_range()) { indices[i]=int(corners.start())+i; }
  auto snapshot=std::make_unique<undo::FaceSetsL0MasterSnapshot>();
  if (const char *error=snapshot->prepare(mmd,mesh.corners_num,indices.as_span())) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  snapshot->modifier_name=mmd.modifier.name;
  IndexMaskMemory memory;
  const IndexMask leaves=bke::pbvh::all_leaf_nodes(*pbvh,memory);
  const Span<bke::pbvh::MeshNode> nodes=pbvh->nodes<bke::pbvh::MeshNode>();
  const IndexMask affected=IndexMask::from_predicate(leaves,memory,[&](const int node) {
    for (const int f:nodes[node].faces()) { if (f==face) { return true; } }return false;
  },exec_mode::grain_size(1));
  if (affected.is_empty()) {
    BKE_report(op->reports,RPT_ERROR,"Selected face is not owned by a PBVH leaf");return OPERATOR_CANCELLED;
  }
  undo::push_begin(*CTX_data_scene(C),ob,op);
  undo::attach_facesets_l0_master(std::move(snapshot));
  undo::push_nodes(depsgraph,ob,affected,undo::Type::FaceSet);
  auto writer=mesh.attributes_for_write().lookup_for_write_span<int>(".sculpt_face_set");
  BLI_assert(writer);
  writer.span[face]=id;writer.finish();
  facesets_rebuild_l0_v01::publish_master(mmd,plan);
  pbvh->tag_face_sets_changed(affected);
  undo::push_end(ob);
  WM_event_add_notifier(C,NC_OBJECT|ND_DRAW,&ob);
  std::printf("[FACESETS_SCULPT_L0_V02] assigned face=%d id=%d pbvh=MESH nodes=%lld master_level=%d\n",
      face,id,static_cast<long long>(affected.size()),mmd.sculpt_face_set_level);
  std::fflush(stdout);
  return OPERATOR_FINISHED;
}
}  // namespace facesets_sculpt_l0_v02
static void SCULPT_OT_facesets_rebuild_l0_assign_v02(wmOperatorType *ot)
{
  ot->name="FaceSets Rebuild Sculpt L0 V02";
  ot->idname="SCULPT_OT_facesets_rebuild_l0_assign_v02";
  ot->description="Assign one base face and its retained master descendants with native Sculpt Undo";
  ot->poll=facesets_sculpt_l0_v02::poll;ot->exec=facesets_sculpt_l0_v02::assign_exec;
  ot->flag=OPTYPE_REGISTER|OPTYPE_UNDO;
  RNA_def_int(ot->srna,"face_index",0,0,INT_MAX,"Base Face","Original face index",0,100);
  RNA_def_int(ot->srna,"face_set_id",7,1,INT_MAX,"Face Set ID","Categorical ID",1,100);
}
