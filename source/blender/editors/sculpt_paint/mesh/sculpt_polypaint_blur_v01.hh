/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "sculpt_polypaint_blur_v01_api.hh"
#include "sculpt_polypaint_blur_v01_math.hh"
#include "sculpt_polypaint_blur_local_v02.hh"
#include <chrono>
#include "sculpt_polypaint_smear_v03_math.hh"
#include "BKE_context.hh"
#include "BLI_path_utils.hh"
#include "BLI_listbase.h"
#include "BKE_modifier.hh"
#include "BKE_report.hh"
#include "BKE_subdiv.hh"
#include "BKE_brush.hh"
#include "BKE_sculpt_facesets_visibility_rc2.hh"
#include "MEM_guardedalloc.h"
#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"
#include <climits>
#include <cstdint>
#include <memory>
#include <vector>

namespace blender::ed::sculpt_paint::color {
static uint64_t blur_v01_dabs=0,smear_v03_dabs=0,color_v03_budget_aborts=0;
static size_t color_v03_work_bytes()
{
  const char *value=std::getenv("BLENDER_SCULPT_POLYPAINT_WORK_MB");
  if (!value || !value[0]) { return size_t(512)*1024*1024; }
  char *end=nullptr;const long mb=std::strtol(value,&end,10);
  if (!end || *end || mb<1 || mb>8192) { return size_t(512)*1024*1024; }
  return size_t(mb)*1024*1024;
}
static void color_v03_budget_abort()
{
  color_v03_budget_aborts++;
  std::printf("[POLYPAINT_COLOR_V03] aborted=local_work_budget master_unchanged=1\n");
  std::fflush(stdout);
}
static bool blur_v01_layout(const Object &object)
{
  const auto *ss=object.runtime->sculpt_session;
  const auto *mmd=ss?ss->multires_modifier:nullptr;
  const Mesh &mesh=*id_cast<const Mesh *>(object.data);
  if (!ss || !mmd || !ss->subdiv_ccg || ss->bm || mesh.id.us!=1 ||
      object.id.lib || mesh.id.lib || object.id.override_library || mesh.id.override_library ||
      object.modifiers.count()!=1 || !mmd->sculpt_color_grids ||
      mmd->sculpt_color_grids_num!=mesh.corners_num || mmd->sculptlvl<1 ||
      mmd->sculptlvl>13 || mmd->sculptlvl!=mmd->totlvl ||
      mmd->sculpt_color_level!=mmd->sculptlvl) { return false; }
  const auto &ccg=*ss->subdiv_ccg;
  const int side=bke::subdiv::grid_size_from_level(mmd->sculptlvl),area=side*side;
  if (ccg.grid_size!=side || ccg.grid_area!=area || ccg.grids_num!=mesh.corners_num ||
      int64_t(ccg.grids_num)*area>INT_MAX || ccg.positions.size()!=int64_t(ccg.grids_num)*area ||
      (!ccg.grid_hidden.is_empty() && (ccg.grid_hidden.size()!=ccg.grids_num || ccg.grid_hidden.group_size()!=area))) { return false; }
  for (int g=0;g<ccg.grids_num;g++) {
    const auto &grid=mmd->sculpt_color_grids[g];
    if (grid.colors && (grid.level!=mmd->sculpt_color_level || grid.totcolor!=area)) { return false; }
  }
  return true;
}
struct BlurV01Stage {
  int grid;
  float (*colors)[4]=nullptr;
  ~BlurV01Stage() { MEM_SAFE_DELETE(colors); }
};
static void do_polypaint_blur_grids_v01(const Depsgraph &depsgraph,const Sculpt &sd,
                                       Object &object,const IndexMask &node_mask,const bool smear=false)
{
  if (!polypaint_blur_v01_enabled() || !blur_v01_layout(object)) { return; }
  if (smear) { smear_v03_dabs++; } else { blur_v01_dabs++; }
  auto &ss=*object.runtime->sculpt_session;
  if (!ss.cache || ss.filter_cache) { return; }
  auto &mmd=*ss.multires_modifier;
  const auto &ccg=*ss.subdiv_ccg;
  const Mesh &mesh=*id_cast<const Mesh *>(object.data);
  const char *attribute=mesh.active_color_attribute;
  if (!attribute || !attribute[0] || (mmd.sculpt_color_attribute[0] &&
      std::strcmp(attribute,mmd.sculpt_color_attribute)!=0)) { return; }
  const CCGKey key=BKE_subdiv_ccg_key_top_level(ccg);
  const int area=key.grid_area,total=ccg.grids_num*area;
  const auto &brush=*BKE_paint_brush_for_read(&sd.paint);
  if (smear && ccg.normals.size()!=ccg.positions.size()) { return; }
  if (smear && brush.smear_deform_type!=BRUSH_SMEAR_DEFORM_DRAG &&
      brush.smear_deform_type!=BRUSH_SMEAR_DEFORM_PINCH &&
      brush.smear_deform_type!=BRUSH_SMEAR_DEFORM_EXPAND) { return; }
  const float strength=std::clamp(std::abs(ss.cache->bstrength),0.0f,1.0f);
  if (!std::isfinite(strength) || strength==0) { return; }
  const auto started=std::chrono::steady_clock::now();
  auto &tree=*bke::object::pbvh_get(object);
  const auto nodes=tree.nodes<bke::pbvh::GridsNode>();
  const size_t work_bytes=color_v03_work_bytes();
  /* Conservative accounting for hash nodes, group fans, source colors, active
   * lists and target maps. This bounds local scratch, not Blender's native Undo. */
  const size_t max_source=work_bytes/512;
  bool budget_exceeded=false;
  std::unordered_map<int,float> influence;
  Vector<float3> positions;
  Vector<float> factors,distances;
  node_mask.foreach_index([&](const int n) {
    if (budget_exceeded) { return; }
    const auto grids=nodes[n].grids();
    if (uint64_t(grids.size())*area>work_bytes/64) { budget_exceeded=true;return; }
    const auto co=gather_grids_positions(ccg,grids,positions);
    calc_factors_common_grids(depsgraph,brush,object,co,nodes[n],factors,distances);
    for (int j=0;j<co.size();j++) {
      const float f=factors[j]*strength;
      if (std::isfinite(f) && f>0) {
        if (influence.size()>=max_source) { budget_exceeded=true;return; }
        influence.emplace(grids[j/area]*area+j%area,std::clamp(f,0.0f,1.0f));
      }
    }
  });
  if (budget_exceeded) { color_v03_budget_abort();return; }
  using namespace blur_local_v02;
  size_t sample_reads=0;
  auto sample=[&](const int i,RGBA &rgba) {
    sample_reads++;
    const auto coord=SubdivCCGCoord::from_index(key,i);
    return bke::multires_layers::polypaint_sample_with_base(mmd,mesh,attribute,coord.grid_index,
        float(coord.x)/float(key.grid_size-1),float(coord.y)/float(key.grid_size-1),rgba.data());
  };
  auto hidden=[&](const int i) {
    return !ccg.grid_hidden.is_empty() && ccg.grid_hidden[i/area][i%area];
  };
  auto duplicates=[&](const int i) {
    std::vector<int> indices;
    const auto coord=SubdivCCGCoord::from_index(key,i);
    if (coord.x==0 || coord.y==0 || coord.x==key.grid_size-1 || coord.y==key.grid_size-1) {
      SubdivCCGNeighbors adjacent;
      BKE_subdiv_ccg_neighbor_coords_get(ccg,coord,true,adjacent);
      for (const auto &copy:adjacent.duplicates()) { indices.push_back(copy.to_index(key)); }
    }
    return indices;
  };
  auto neighbors=[&](const int i) {
    std::vector<int> indices;SubdivCCGNeighbors adjacent;
    BKE_subdiv_ccg_neighbor_coords_get(ccg,SubdivCCGCoord::from_index(key,i),true,adjacent);
    for (const auto &coord:adjacent.unique()) { indices.push_back(coord.to_index(key)); }
    return indices;
  };
  Result result;
  const float3 drag=brush.stroke_method==BRUSH_STROKE_ANCHORED?ss.cache->grab_delta_symm:
      ss.cache->location_symm-ss.cache->last_location_symm;
  auto weight=[&](const int i,const int n) {
    if (!smear) { return 1.0; }
    float3 motion=drag;
    if (brush.smear_deform_type==BRUSH_SMEAR_DEFORM_PINCH) { motion=ss.cache->location_symm-ccg.positions[i]; }
    else if (brush.smear_deform_type==BRUSH_SMEAR_DEFORM_EXPAND) { motion=ccg.positions[i]-ss.cache->location_symm; }
    if (ss.cache->toggle_settings.invert) { motion=-motion; }
    const float3 edge=ccg.positions[n]-ccg.positions[i];
    const float3 normal=ccg.normals[i];
    return smear_v03_math::weight({double(motion.x),double(motion.y),double(motion.z)},
        {double(edge.x),double(edge.y),double(edge.z)},
        {double(normal.x),double(normal.y),double(normal.z)});
  };
  if (!calculate(influence,sample,duplicates,neighbors,hidden,result,weight,smear,max_source)) {
    if (result.source.size()>=max_source) { color_v03_budget_abort(); }
    return;
  }
  int64_t changed_samples=0;
  std::map<int,std::unique_ptr<BlurV01Stage>> staged;
  /* Allocate only changed grids, preserving all their untouched samples.
   * No master data is published until the complete staging succeeds. */
  for (const auto &entry:result.writes) {
    const int i=entry.first,g=i/area,j=i%area;
    auto &stage=staged[g];
    if (!stage) {
      const uint64_t stage_bytes=uint64_t(staged.size())*uint64_t(area)*sizeof(float[4]);
      if (uint64_t(result.source.size())*512+stage_bytes>work_bytes) {
        color_v03_budget_abort();return;
      }
      stage=std::make_unique<BlurV01Stage>();stage->grid=g;
      stage->colors=MEM_new_array_uninitialized<float[4]>(area,"Polypaint Blur V02 local stage");
      for (int k=0;k<area;k++) {
        RGBA rgba;
        const auto saved=result.source.find(g*area+k);
        if (saved!=result.source.end()) { rgba=saved->second; }
        else if (!sample(g*area+k,rgba)) { return; }
        for (int c=0;c<4;c++) {
          if (!std::isfinite(rgba[c])) { return; }
          stage->colors[k][c]=rgba[c];
        }
      }
    }
    for (int c=0;c<3;c++) { stage->colors[j][c]=entry.second[c]; }
    changed_samples++;
  }
  /* All reads, computation and validation finish before any master is changed.
   * Every modified grid belongs to captured node_mask: a seam shared with an
   * uncaptured/hidden/masked copy has shared influence zero and remains intact. */
  for (auto &entry:staged) {
    auto &stage=entry.second;
    auto &grid=mmd.sculpt_color_grids[stage->grid];
    MEM_SAFE_DELETE(grid.colors);grid.colors=stage->colors;stage->colors=nullptr;
    grid.level=mmd.sculpt_color_level;grid.totcolor=area;
  }
  if (!staged.empty()) { tree.tag_attribute_changed(node_mask,attribute); }
  const double elapsed_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  std::printf("[POLYPAINT_COLOR_LOCAL_V03] changed=%lld grids=%llu sampled=%llu total=%d groups=%llu ms=%.3f level=%d smear=%d\n",
      (long long)changed_samples,(unsigned long long)staged.size(),
      (unsigned long long)sample_reads,total,(unsigned long long)result.groups,elapsed_ms,int(mmd.sculptlvl),int(smear));
  std::fflush(stdout);
}

/* Internal, opt-in audit. It exports real live/master colors and CCG geometry,
 * without changing them. ACTIVATE creates only a local brush for the test/UI. */
static bool blur_v01_probe_poll(bContext *C)
{
  if (!polypaint_blur_v01_enabled() || !sculpt_mode_poll(C)) { return false; }
  const auto *ss=CTX_data_active_object(C)->runtime->sculpt_session;
  return ss && !ss->cache && !ss->filter_cache && ((ss->subdiv_ccg && ss->multires_modifier) || (polypaint_v04_enabled() && color_v04_modifier(*CTX_data_active_object(C))));
}
static wmOperatorStatus blur_v01_probe_exec(bContext *C,wmOperator *op)
{
  Object &object=*CTX_data_active_object(C);
  const int action=RNA_enum_get(op->ptr,"action");
  if (action==1 || action==2 || action==3) {
    Brush *brush=BKE_brush_add(CTX_data_main(C),action==1?"Polypaint HR Blur V03":(action==3?"Polypaint HR Smear V03":"Polypaint Test Paint"),OB_MODE_SCULPT);
    brush->sculpt_brush_type=action==1?SCULPT_BRUSH_TYPE_BLUR:(action==3?SCULPT_BRUSH_TYPE_SMEAR:SCULPT_BRUSH_TYPE_PAINT);
    brush->smear_deform_type=BRUSH_SMEAR_DEFORM_DRAG;
    brush->stroke_method=BRUSH_STROKE_SPACE;brush->size=90;brush->alpha=.5f;
    Paint *paint=BKE_paint_get_active_from_context(C);
    if (!paint || !WM_toolsystem_activate_brush_and_tool(C,paint,brush)) { return OPERATOR_CANCELLED; }
    return OPERATOR_FINISHED;
  }
  if(polypaint_v04_enabled())return color_v04_export(C,op)?OPERATOR_FINISHED:OPERATOR_CANCELLED;
  const auto &ss=*object.runtime->sculpt_session;
  const auto &ccg=*ss.subdiv_ccg;
  const auto &mesh=*id_cast<const Mesh *>(object.data);
  const auto *mmd=ss.multires_modifier;
  const int64_t total=int64_t(ccg.grids_num)*ccg.grid_area;
  const bool summary=action==4;
  const bool no_colors=!mesh.active_color_attribute || !mesh.active_color_attribute[0];
  if ((!summary && total>1048576) || total>INT_MAX || total<=0 || (!summary && no_colors) ||
      (mmd->sculpt_color_grids && mmd->sculpt_color_grids_num!=ccg.grids_num)) { return OPERATOR_CANCELLED; }
  char path[FILE_MAX];RNA_string_get(op->ptr,"filepath",path);
  FILE *out=std::fopen(path,"wb");if (!out) { return OPERATOR_CANCELLED; }
  std::fprintf(out,"{\"blur_backend\":3,\"smear_dabs\":%llu,\"budget_aborts\":%llu,\"blur_dabs\":%llu,\"level\":%d,\"master_level\":%d,\"present\":[",(unsigned long long)smear_v03_dabs,(unsigned long long)color_v03_budget_aborts,(unsigned long long)blur_v01_dabs,int(mmd->sculptlvl),int(mmd->sculpt_color_level));
  for (int g=0;g<ccg.grids_num;g++) { std::fprintf(out,"%s%d",g?",":"",int(mmd->sculpt_color_grids && mmd->sculpt_color_grids[g].colors)); }
  auto hash_bytes=[](uint64_t &hash,const void *data,const size_t bytes) {
    const auto *p=static_cast<const unsigned char *>(data);
    for (size_t i=0;i<bytes;i++) { hash^=p[i];hash*=1099511628211ULL; }
  };
  uint64_t colors_hash=1469598103934665603ULL,positions_hash=1469598103934665603ULL,mask_hash=1469598103934665603ULL;
  bool nonuniform=false;float first_color[4]={};
  std::fprintf(out,"],\"colors\":[");
  for (int g=0;!no_colors && g<ccg.grids_num;g++) {
    for (int j=0;j<ccg.grid_area;j++) {
      float color[4]={};const bool sampled=bke::multires_layers::polypaint_sample_with_base(*mmd,mesh,mesh.active_color_attribute,g,
          float(j%ccg.grid_size)/float(ccg.grid_size-1),float(j/ccg.grid_size)/float(ccg.grid_size-1),color);
      if (!sampled) { std::fclose(out);return OPERATOR_CANCELLED; }
      hash_bytes(colors_hash,color,sizeof(color));
      if (!g && !j) { std::memcpy(first_color,color,sizeof(color)); }
      else { nonuniform|=std::memcmp(first_color,color,sizeof(color))!=0; }
      if (!summary) { std::fprintf(out,"%s[%.9g,%.9g,%.9g,%.9g]",g||j?",":"",color[0],color[1],color[2],color[3]); }
    }
  }
  std::fprintf(out,"],\"positions\":[");
  for (int i=0;i<total;i++) { const auto &p=ccg.positions[i];hash_bytes(positions_hash,&p.x,sizeof(float)*3);if (!summary) { std::fprintf(out,"%s[%.9g,%.9g,%.9g]",i?",":"",p.x,p.y,p.z); } }
  std::fprintf(out,"],\"hidden\":[");bool first=true;
  for (int i=0;i<total;i++) { if (!ccg.grid_hidden.is_empty() && ccg.grid_hidden[i/ccg.grid_area][i%ccg.grid_area]) { std::fprintf(out,"%s%d",first?"":",",i);first=false; } }
  uint64_t face_hash=1469598103934665603ULL,layer_hash=1469598103934665603ULL;
  const auto base_ids=*mesh.attributes().lookup_or_default<int>(".sculpt_face_set",bke::AttrDomain::Face,1);
  for (int f=0;f<mesh.faces_num;f++) { const int id=base_ids[f];hash_bytes(face_hash,&id,sizeof(id)); }
  if (bke::multires_layers::faceset_master_valid(*mmd,mesh.corners_num)) {
    const int fs_side=bke::subdiv::grid_size_from_level(mmd->sculpt_face_set_level);
    for (int g=0;g<mmd->sculpt_face_set_grids_num;g++) {
      const auto &grid=mmd->sculpt_face_set_grids[g];
      for (int j=0;j<fs_side*fs_side;j++) { const int id=grid.ids?grid.ids[j]:grid.uniform_id;hash_bytes(face_hash,&id,sizeof(id)); }
    }
  }
  for (const MultiresSculptLayer &layer:mmd->sculpt_layers) {
    hash_bytes(layer_hash,layer.name,sizeof(layer.name));hash_bytes(layer_hash,&layer.strength,sizeof(layer.strength));
    if (layer.base_deltas && layer.base_verts_num>0) { hash_bytes(layer_hash,layer.base_deltas,size_t(layer.base_verts_num)*sizeof(float[3])); }
    if (layer.disps) {
      for (int g=0;g<layer.grids_num;g++) {
        const auto &grid=layer.disps[g];
        if (grid.disps && grid.totdisp>0) { hash_bytes(layer_hash,grid.disps,size_t(grid.totdisp)*sizeof(float[3])); }
      }
    }
  }
  std::fprintf(out,"],\"face_sets_hash\":\"%llu\",\"layer_hash\":\"%llu\",\"mask\":[",(unsigned long long)face_hash,(unsigned long long)layer_hash);

  for (int i=0;i<ccg.masks.size();i++) { const float value=ccg.masks[i];hash_bytes(mask_hash,&value,sizeof(value));if (!summary) { std::fprintf(out,"%s%.9g",i?",":"",value); } }
  std::fprintf(out,"],\"sample_count\":%lld,\"colors_hash\":\"%llu\",\"positions_hash\":\"%llu\",\"mask_hash\":\"%llu\",\"nonuniform\":%s}\n",
      (long long)total,(unsigned long long)colors_hash,(unsigned long long)positions_hash,(unsigned long long)mask_hash,nonuniform?"true":"false");const bool failed=std::ferror(out)!=0;const int closed=std::fclose(out);
  return failed || closed?OPERATOR_CANCELLED:OPERATOR_FINISHED;
}
void SCULPT_OT_polypaint_blur_probe_v01(wmOperatorType *ot)
{
  ot->name="Polypaint Blur V01 Audit";ot->idname="SCULPT_OT_polypaint_blur_probe_v01";
  ot->poll=blur_v01_probe_poll;ot->exec=blur_v01_probe_exec;ot->flag=OPTYPE_INTERNAL;
  static const EnumPropertyItem actions[]={{0,"EXPORT",0,"Export","Read-only color snapshot"},
      {1,"BLUR",0,"Blur","Activate local HR Blur brush"},{2,"PAINT",0,"Paint","Activate local Paint brush"},
      {3,"SMEAR",0,"Smear","Activate HR color Smear brush"},
      {4,"SUMMARY",0,"Summary","Read-only fingerprints, including when no color attribute exists"},
      {0,nullptr,0,nullptr,nullptr}};
  RNA_def_enum(ot->srna,"action",actions,0,"Action","");
  RNA_def_string_file_path(ot->srna,"filepath",nullptr,FILE_MAX,"File Path","Diagnostic JSON");
}
}

#include "sculpt_polypaint_v04.hh"
