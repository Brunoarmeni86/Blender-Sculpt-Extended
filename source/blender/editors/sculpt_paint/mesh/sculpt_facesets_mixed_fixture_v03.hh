/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diagnostic fixture only; included inside blender::ed::sculpt_paint. */
namespace facesets_mixed_fixture_v03 {
inline int sample_id(const int corner, const int sample)
{
  return 100 + corner * 31 + ((sample * 7 + sample / 5) % 23);
}
inline bool enabled()
{
  const char *v = BLI_getenv("BLENDER_FACESETS_MIXED_TEST_V03");
  return v && v[0]=='1' && v[1]=='\0';
}
inline bool poll(bContext *C)
{
  return enabled() && facesets_rebuild_l0_v01::audit_poll(C);
}
inline const char *fixture_error(Object &ob, Mesh &mesh, MultiresModifierData &mmd)
{
  if (ob.id.lib || ob.id.override_library || mesh.id.lib || mesh.id.override_library ||
      mesh.id.us!=1 || mesh.key || ob.modifiers.count()!=1 ||
      mesh.verts_num!=8 || mesh.faces_num!=6 || mesh.corners_num!=24 || mmd.totlvl!=3 ||
      !STREQ(ob.id.name+2,"FaceSets_Rebuild_L0_Test")) {
    return "Diagnostic requires the generated single-user cube fixture at maximum level 3";
  }
  if (ob.runtime->sculpt_session && (ob.runtime->sculpt_session->cache || ob.runtime->sculpt_session->filter_cache)) {
    return "Finish the stroke before the diagnostic";
  }
  return facesets_rebuild_l0_v01::binding_error(mmd,mesh);
}
inline wmOperatorStatus seed_exec(bContext *C, wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  auto &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  if (const char *error=fixture_error(ob,mesh,mmd)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  if (ob.mode!=OB_MODE_OBJECT || ob.runtime->sculpt_session || mmd.lvl || mmd.sculptlvl ||
      mmd.sculpt_face_set_grids || mmd.sculpt_face_set_grids_num || mmd.sculpt_face_set_level) {
    BKE_report(op->reports,RPT_ERROR,"Seed only accepts a fresh Object Mode fixture without master");
    return OPERATOR_CANCELLED;
  }
  const VArraySpan<int> base=*mesh.attributes().lookup_or_default<int>(".sculpt_face_set",bke::AttrDomain::Face,0);
  if (base.size()!=6) { return OPERATOR_CANCELLED; }
  for (const int value:base) { if (value!=1) { return OPERATOR_CANCELLED; } }
  struct Owned {
    MultiresFaceSetGrid *grids=nullptr;
    ~Owned() {
      if (grids) {
        for (int i=0;i<24;i++) { if (grids[i].ids) { MEM_delete(grids[i].ids); } }
        MEM_delete(grids);
      }
    }
  } staged;
  staged.grids=MEM_new_array<MultiresFaceSetGrid>(24,"FaceSets mixed fixture descriptors");
  for (int corner=0;corner<24;corner++) {
    auto &grid=staged.grids[corner];
    grid.level=3;grid.totid=25;grid.uniform_id=1;
    grid.ids=MEM_new_array<int>(25,"FaceSets mixed fixture IDs");
    for (int i=0;i<25;i++) { grid.ids[i]=sample_id(corner,i); }
  }
  mmd.sculpt_face_set_grids=staged.grids;staged.grids=nullptr;
  mmd.sculpt_face_set_grids_num=24;mmd.sculpt_face_set_level=3;
  DEG_id_tag_update(&ob.id,ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_OBJECT|ND_MODIFIER,&ob);
  std::printf("[FACESETS_MIXED_V03] seeded 24 dense grids, 600 deterministic IDs\n");
  std::fflush(stdout);
  return OPERATOR_FINISHED;
}
inline wmOperatorStatus audit_exec(bContext *C, wmOperator *op)
{
  Object &ob=*CTX_data_active_object(C);
  Mesh &mesh=*id_cast<Mesh *>(ob.data);
  auto &mmd=*facesets_rebuild_l0_v01::original_modifier(ob);
  if (const char *error=fixture_error(ob,mesh,mmd)) {
    BKE_report(op->reports,RPT_ERROR,error);return OPERATOR_CANCELLED;
  }
  const int target=RNA_int_get(op->ptr,"target_id");
  if (!mmd.sculpt_face_set_grids || mmd.sculpt_face_set_grids_num!=24 || mmd.sculpt_face_set_level!=3) {
    BKE_report(op->reports,RPT_ERROR,"Mixed fixture master missing or layout changed");return OPERATOR_CANCELLED;
  }
  const VArraySpan<int> base=*mesh.attributes().lookup_or_default<int>(".sculpt_face_set",bke::AttrDomain::Face,0);
  if (base.size()!=6) { return OPERATOR_CANCELLED; }
  uint64_t mismatch=0,checked=0,dense=0;
  for (const int face:mesh.faces().index_range()) {
    const bool replaced=face==0 && target>0;
    mismatch+=base[face]!=(replaced?target:1);
    for (const int corner:mesh.faces()[face]) {
      const auto &grid=mmd.sculpt_face_set_grids[corner];
      mismatch+=grid.level!=3;
      if (replaced) {
        mismatch+=grid.ids!=nullptr || grid.totid!=0 || grid.uniform_id!=target;
      }
      else {
        mismatch+=grid.uniform_id!=1;
        if (!grid.ids || grid.totid!=25) { mismatch++;continue; }
        dense++;
        for (int i=0;i<25;i++) { mismatch+=grid.ids[i]!=sample_id(corner,i);checked++; }
      }
    }
  }
  std::printf("[FACESETS_MIXED_V03_AUDIT] target=%d dense=%llu samples=%llu mismatches=%llu mode=%d\n",
      target,static_cast<unsigned long long>(dense),static_cast<unsigned long long>(checked),
      static_cast<unsigned long long>(mismatch),int(ob.mode));
  std::fflush(stdout);
  if (mismatch) { BKE_report(op->reports,RPT_ERROR,"Mixed master/base IDs differ from exact fixture");return OPERATOR_CANCELLED; }
  return OPERATOR_FINISHED;
}
} // namespace facesets_mixed_fixture_v03
static void OBJECT_OT_facesets_mixed_seed_v03(wmOperatorType *ot)
{
  ot->name="FaceSets Mixed Fixture Seed V03";ot->idname="OBJECT_OT_facesets_mixed_seed_v03";
  ot->description="Diagnostic only: populate a fresh generated test cube with deterministic dense IDs";
  ot->poll=facesets_mixed_fixture_v03::poll;ot->exec=facesets_mixed_fixture_v03::seed_exec;
  ot->flag=OPTYPE_INTERNAL;
}
static void OBJECT_OT_facesets_mixed_audit_v03(wmOperatorType *ot)
{
  ot->name="FaceSets Mixed Fixture Audit V03";ot->idname="OBJECT_OT_facesets_mixed_audit_v03";
  ot->description="Read every stored test ID and metadata without modifying the master";
  ot->poll=facesets_mixed_fixture_v03::poll;ot->exec=facesets_mixed_fixture_v03::audit_exec;
  ot->flag=OPTYPE_INTERNAL;
  RNA_def_int(ot->srna,"target_id",0,0,INT_MAX,"Target ID","Zero expects original mixed pattern; positive expects uniform face zero",0,100);
}
