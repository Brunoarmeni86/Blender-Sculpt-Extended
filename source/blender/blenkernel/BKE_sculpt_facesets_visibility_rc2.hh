/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <vector>
#include <memory>
#include <string>
#include <cstdlib>
#include <cstring>
#include "BLI_bitmap.h"
#include "BKE_customdata.hh"
#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_sculpt_facesets_rc1.hh"
#include "BKE_subdiv_ccg.hh"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "MEM_guardedalloc.h"
namespace blender::bke::facesets_visibility_rc2 {
inline bool enabled()
{
  const char *v=std::getenv("BLENDER_FACESETS_VISIBILITY_RC2");
  return facesets_rc1::enabled() && v && std::strcmp(v,"1")==0;
}
inline const MultiresModifierData *modifier(const Object &ob)
{
  if (!ob.modifiers.is_single()) { return nullptr; }
  const auto *md=static_cast<const ModifierData *>(ob.modifiers.first);
  if (md->type!=eModifierType_Multires || !(md->mode&eModifierMode_Realtime) ||
      (md->mode&eModifierMode_DisableTemporary)) { return nullptr; }
  return reinterpret_cast<const MultiresModifierData *>(md);
}
inline bool layout(const Mesh &mesh,const MultiresModifierData &mmd)
{
  if (mmd.totlvl<1 || mmd.totlvl>8 || mesh.corners_num<=0) { return false; }
  const MDisps *grids=static_cast<const MDisps *>(CustomData_get_layer(&mesh.corner_data,CD_MDISPS));
  if (!grids) { return false; }
  const int side=(1<<(mmd.totlvl-1))+1;
  for (int c=0;c<mesh.corners_num;c++) {
    if (grids[c].level!=mmd.totlvl || grids[c].totdisp!=side*side) { return false; }
  }
  return true;
}
/* Existing MDisps::hidden is serialized natively. No second Face Set ID master. */
inline bool ensure(Mesh &mesh,const MultiresModifierData &mmd)
{
  if (!layout(mesh,mmd)) { return false; }
  auto *grids=static_cast<MDisps *>(CustomData_get_layer_for_write(&mesh.corner_data,CD_MDISPS,mesh.corners_num));
  const auto hidden=*mesh.attributes().lookup_or_default<bool>(".hide_poly",AttrDomain::Face,false);
  const auto faces=mesh.faces();
  for (const int f:faces.index_range()) {
    for (const int c:faces[f]) {
      if (grids[c].hidden) { continue; }
      grids[c].hidden=BLI_BITMAP_NEW(grids[c].totdisp,"Face Sets persistent HR visibility");
      if (hidden[f]) {
        for (int i=0;i<grids[c].totdisp;i++) { BLI_BITMAP_ENABLE(grids[c].hidden,i); }
      }
    }
  }
  return true;
}
inline bool project(const Mesh &mesh,const MultiresModifierData &mmd,SubdivCCG &ccg)
{
  if (!layout(mesh,mmd) || ccg.grids_num!=mesh.corners_num) { return false; }
  const auto *grids=static_cast<const MDisps *>(CustomData_get_layer(&mesh.corner_data,CD_MDISPS));
  /* FACESETS_RC9_MIXED_HIDDEN_FIX_B: new corners may have implicit visibility.
   * A missing bitmap must not discard persistent visibility on all retained grids. */
  const auto coarse_hidden=*mesh.attributes().lookup_or_default<bool>(".hide_poly",AttrDomain::Face,false);
  const auto corner_faces=mesh.corner_to_face_map();
  const int high=(1<<(mmd.totlvl-1))+1,low=ccg.grid_size;
  if (low<2 || low>high || (high-1)%(low-1)) { return false; }
  const int stride=(high-1)/(low-1);
  auto &bits=BKE_subdiv_ccg_grid_hidden_ensure(ccg);
  for (int c=0;c<mesh.corners_num;c++) {
    for (int y=0;y<low;y++) {
      for (int x=0;x<low;x++) {
        bits[c][y*low+x].set(grids[c].hidden ? BLI_BITMAP_TEST_BOOL(grids[c].hidden,(y*stride)*high+x*stride) : coarse_hidden[corner_faces[c]]);
      }
    }
  }
  return true;
}
inline bool change(Mesh &mesh,const MultiresModifierData &mmd,const int mode,
                   const int id,const bool show_all)
{
  if (!ensure(mesh,mmd)) { return false; }
  auto *grids=static_cast<MDisps *>(CustomData_get_layer_for_write(&mesh.corner_data,CD_MDISPS,mesh.corners_num));
  for (int c=0;c<mesh.corners_num;c++) {
    const auto &ids=mmd.sculpt_face_set_grids[c];
    for (int i=0;i<grids[c].totdisp;i++) {
      const bool matches=(ids.ids?ids.ids[i]:ids.uniform_id)==id;
      bool hide=BLI_BITMAP_TEST_BOOL(grids[c].hidden,i);
      if (show_all) { hide=false; }
      else if (mode==0) { hide=!matches; }
      else if (mode==1 && matches) { hide=false; }
      else if (mode==2 && matches) { hide=true; }
      BLI_BITMAP_SET(grids[c].hidden,i,hide);
    }
  }
  return true;
}
inline bool any_hidden(const Mesh &mesh,const MultiresModifierData &mmd)
{
  if (!layout(mesh,mmd)) { return false; }
  const auto *grids=static_cast<const MDisps *>(CustomData_get_layer(&mesh.corner_data,CD_MDISPS));
  for (int c=0;c<mesh.corners_num;c++) {
    if (!grids[c].hidden) { continue; }
    for (int i=0;i<grids[c].totdisp;i++) {
      if (BLI_BITMAP_TEST_BOOL(grids[c].hidden,i)) { return true; }
    }
  }
  return false;
}
inline int face_id(const Mesh &mesh,const MultiresModifierData &mmd,const int face)
{
  const auto &grid=mmd.sculpt_face_set_grids[mesh.faces()[face].start()];
  return grid.ids?grid.ids[0]:grid.uniform_id;
}
inline bool project_faces(Mesh &mesh,const MultiresModifierData &mmd)
{
  if (!layout(mesh,mmd)) { return false; }
  const auto *grids=static_cast<const MDisps *>(CustomData_get_layer(&mesh.corner_data,CD_MDISPS));

  auto hidden=mesh.attributes_for_write().lookup_or_add_for_write_span<bool>(".hide_poly",AttrDomain::Face);
  for (const int f:mesh.faces().index_range()) {
    const auto *bitmap=grids[mesh.faces()[f].start()].hidden;
    if (bitmap) { hidden.span[f]=BLI_BITMAP_TEST_BOOL(bitmap,0); }
  }
  hidden.finish();
  mesh_hide_face_flush(mesh);
  return true;
}
/* Import only cells whose native visibility changed. Unaffected fine bits
 * survive an edit made at a lower level. mode: 0=delta, 1=show all, 2=hide all, 3=invert. */
inline bool import_native(Mesh &mesh,const MultiresModifierData &mmd,const SubdivCCG *ccg,
                          const int mode)
{
  if (!ensure(mesh,mmd)) { return false; }
  auto *grids=static_cast<MDisps *>(CustomData_get_layer_for_write(&mesh.corner_data,CD_MDISPS,mesh.corners_num));
  if (mode!=0) {
    for (int c=0;c<mesh.corners_num;c++) {
      for (int i=0;i<grids[c].totdisp;i++) {
        const bool value=mode==2 || (mode==3 && !BLI_BITMAP_TEST_BOOL(grids[c].hidden,i));
        BLI_BITMAP_SET(grids[c].hidden,i,value);
      }
    }
    return true;
  }
  if (!ccg) {
    const auto hidden=*mesh.attributes().lookup_or_default<bool>(".hide_poly",AttrDomain::Face,false);
    for (const int f:mesh.faces().index_range()) {
      const int first=mesh.faces()[f].start();
      if (BLI_BITMAP_TEST_BOOL(grids[first].hidden,0)==hidden[f]) { continue; }
      for (const int c:mesh.faces()[f]) {
        for (int i=0;i<grids[c].totdisp;i++) { BLI_BITMAP_SET(grids[c].hidden,i,hidden[f]); }
      }
    }
    return true;
  }
  const int high=(1<<(mmd.totlvl-1))+1,low=ccg->grid_size;
  if (ccg->grids_num!=mesh.corners_num || low<2 || low>high || (high-1)%(low-1)) { return false; }
  const int stride=(high-1)/(low-1);
  std::vector<unsigned char> old(low*low);
  for (int c=0;c<mesh.corners_num;c++) {
    for (int y=0;y<low;y++) {
      for (int x=0;x<low;x++) { old[y*low+x]=BLI_BITMAP_TEST_BOOL(grids[c].hidden,y*stride*high+x*stride); }
    }
    for (int y=0;y<high;y++) {
      const int ly=(y+(stride-1)/2)/stride;
      for (int x=0;x<high;x++) {
        const int lx=(x+(stride-1)/2)/stride,index=ly*low+lx;
        const bool value=!ccg->grid_hidden.is_empty() && ccg->grid_hidden[c][index];
        if (value!=bool(old[index])) { BLI_BITMAP_SET(grids[c].hidden,y*high+x,value); }
      }
    }
  }
  return true;
}
struct Snapshot {
  std::vector<BLI_bitmap *> saved;
  std::string modifier_name;
  int level=0,area=0;
  ~Snapshot() { for (auto *p:saved) { MEM_SAFE_DELETE(p); } }
  bool prepare(Mesh &mesh,const MultiresModifierData &mmd)
  {
    if (!ensure(mesh,mmd)) { return false; }
    modifier_name=mmd.modifier.name;level=mmd.totlvl;area=((1<<(level-1))+1)*((1<<(level-1))+1);
    const auto *grids=static_cast<const MDisps *>(CustomData_get_layer(&mesh.corner_data,CD_MDISPS));
    saved.reserve(mesh.corners_num);
    for (int c=0;c<mesh.corners_num;c++) { saved.push_back(MEM_dupalloc(grids[c].hidden)); }
    return true;
  }
  bool valid(const Mesh &mesh,const MultiresModifierData &mmd) const
  {
    return modifier_name==mmd.modifier.name && level==mmd.totlvl &&
        int(saved.size())==mesh.corners_num && layout(mesh,mmd);
  }
  bool exchange(Mesh &mesh,const MultiresModifierData &mmd)
  {
    if (!valid(mesh,mmd)) { return false; }
    auto *grids=static_cast<MDisps *>(CustomData_get_layer_for_write(&mesh.corner_data,CD_MDISPS,mesh.corners_num));
    for (int c=0;c<mesh.corners_num;c++) { std::swap(saved[c],grids[c].hidden); }
    return true;
  }
  size_t memory_bytes() const { return sizeof(*this)+modifier_name.capacity()+saved.capacity()*sizeof(BLI_bitmap *)+saved.size()*((area+7)/8+sizeof(BLI_bitmap)); }
};
}
