/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include "MEM_guardedalloc.h"
#include "DNA_modifier_types.h"
#include "BLI_span.hh"
namespace blender::ed::sculpt_paint::undo {
/* BEGIN_TESTABLE_SNAPSHOT */
struct FaceSetsL0MasterSnapshot {
  struct Slot { int corner=-1; MultiresFaceSetGrid grid; };
  std::string modifier_name;
  int corners_num=0,top_level=0;
  bool whole_swap=false;
  MultiresFaceSetGrid *whole=nullptr;
  int whole_count=0,whole_level=0;
  size_t payload_bytes=0;
  std::vector<Slot> slots;
  FaceSetsL0MasterSnapshot()=default;
  FaceSetsL0MasterSnapshot(const FaceSetsL0MasterSnapshot &)=delete;
  FaceSetsL0MasterSnapshot &operator=(const FaceSetsL0MasterSnapshot &)=delete;
  ~FaceSetsL0MasterSnapshot() {
    if (whole) {
      for (int i=0;i<whole_count;i++) { if (whole[i].ids) { MEM_delete(whole[i].ids); } }
      MEM_delete(whole);
    }
    for (Slot &slot:slots) { if (slot.grid.ids) { MEM_delete(slot.grid.ids); } }
  }
  const char *validate_live(const MultiresModifierData &mmd,const int corners) const {
    if (corners!=corners_num || mmd.totlvl!=top_level)
    { return "FaceSets L0 Undo topology/maximum level changed"; }
    if (!mmd.sculpt_face_set_grids) {
      if (mmd.sculpt_face_set_grids_num!=0 || mmd.sculpt_face_set_level!=0 || !whole_swap || !whole)
      { return "FaceSets L0 Undo found an unexpected absent master"; }
      return nullptr;
    }
    if (whole_swap && whole) { return "FaceSets L0 Undo would replace two present masters"; }
    if (mmd.sculpt_face_set_grids_num!=corners_num || mmd.sculpt_face_set_level!=top_level)
    { return "FaceSets L0 Undo master layout mismatch"; }
    const int side=(1<<(top_level-1))+1,area=side*side;
    for (int i=0;i<corners_num;i++) {
      const MultiresFaceSetGrid &grid=mmd.sculpt_face_set_grids[i];
      if (grid.level!=top_level || (grid.ids?grid.totid!=area:grid.totid!=0))
      { return "FaceSets L0 Undo invalid grid"; }
    }
    return nullptr;
  }
  const char *prepare(const MultiresModifierData &mmd,const int corners,const Span<int> indices) {
    if (corners_num || whole || !slots.empty() || corners<=0 || indices.is_empty() ||
        mmd.totlvl<1 || mmd.totlvl>13) { return "Invalid FaceSets L0 snapshot request"; }
    std::vector<int> sorted(indices.begin(),indices.end());
    std::sort(sorted.begin(),sorted.end());
    for (size_t i=0;i<sorted.size();i++) {
      if (sorted[i]<0 || sorted[i]>=corners || (i && sorted[i]==sorted[i-1]))
      { return "Invalid/duplicate snapshot corner"; }
    }
    corners_num=corners;top_level=int(mmd.totlvl);
    if (!mmd.sculpt_face_set_grids) {
      if (mmd.sculpt_face_set_grids_num || mmd.sculpt_face_set_level)
      { return "Invalid absent master"; }
      whole_swap=true;
      return nullptr;
    }
    if (const char *error=validate_live(mmd,corners)) { return error; }
    slots.reserve(sorted.size());
    for (const int corner:sorted) {
      slots.push_back({});Slot &slot=slots.back();slot.corner=corner;
      slot.grid=mmd.sculpt_face_set_grids[corner];slot.grid.ids=nullptr;
      const MultiresFaceSetGrid &source=mmd.sculpt_face_set_grids[corner];
      if (source.ids) {
        slot.grid.ids=MEM_new_array<int>(source.totid,"FaceSets L0 sparse Undo IDs");
        std::copy_n(source.ids,source.totid,slot.grid.ids);
        payload_bytes+=size_t(source.totid)*sizeof(int);
      }
    }
    return nullptr;
  }
  const char *extend(const MultiresModifierData &mmd,const int corners,const Span<int> indices) {
    if (whole_swap || !mmd.sculpt_face_set_grids) { return "Cannot extend an absent-master snapshot"; }
    if (const char *error=validate_live(mmd,corners)) { return error; }
    std::vector<int> sorted(indices.begin(),indices.end());
    std::sort(sorted.begin(),sorted.end());
    sorted.erase(std::unique(sorted.begin(),sorted.end()),sorted.end());
    for (const int corner:sorted) {
      if (corner<0 || corner>=corners_num) { return "Invalid additional snapshot corner"; }
    }
    std::vector<Slot> added;
    for (const int corner:sorted) {
      const auto found=std::lower_bound(slots.begin(),slots.end(),corner,
          [](const Slot &slot,const int index) { return slot.corner<index; });
      if (found!=slots.end() && found->corner==corner) { continue; }
      added.push_back({});Slot &slot=added.back();slot.corner=corner;
      const MultiresFaceSetGrid &source=mmd.sculpt_face_set_grids[corner];
      slot.grid=source;slot.grid.ids=nullptr;
      if (source.ids) {
        slot.grid.ids=MEM_new_array<int>(source.totid,"FaceSets RC1 L0 Undo IDs");
        std::copy_n(source.ids,source.totid,slot.grid.ids);
        payload_bytes+=size_t(source.totid)*sizeof(int);
      }
    }
    slots.insert(slots.end(),added.begin(),added.end());
    std::sort(slots.begin(),slots.end(),[](const Slot &a,const Slot &b) {return a.corner<b.corner;});
    return nullptr;
  }
  /* Caller validates before changing base IDs. This exchange cannot allocate. */
  void exchange(MultiresModifierData &mmd) {
    if (whole_swap) {
      std::swap(whole,mmd.sculpt_face_set_grids);
      std::swap(whole_count,mmd.sculpt_face_set_grids_num);
      std::swap(whole_level,mmd.sculpt_face_set_level);
      payload_bytes=whole?size_t(whole_count)*sizeof(MultiresFaceSetGrid):0;
      if (whole) { for (int i=0;i<whole_count;i++) { if (whole[i].ids) { payload_bytes+=size_t(whole[i].totid)*sizeof(int); } } }
    }
    else {
      payload_bytes=0;
      for (Slot &slot:slots) {
        std::swap(slot.grid,mmd.sculpt_face_set_grids[slot.corner]);
        if (slot.grid.ids) { payload_bytes+=size_t(slot.grid.totid)*sizeof(int); }
      }
    }
  }
  size_t memory_bytes() const {
    return sizeof(*this)+modifier_name.capacity()+slots.capacity()*sizeof(Slot)+payload_bytes;
  }
};
/* END_TESTABLE_SNAPSHOT */
}  // namespace blender::ed::sculpt_paint::undo
