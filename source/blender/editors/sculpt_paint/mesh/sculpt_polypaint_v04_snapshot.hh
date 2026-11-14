/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "sculpt_polypaint_v04_api.hh"
#include "BLI_array.hh"
#include "BLI_vector.hh"

#include <memory>
#include <cstring>
#include <string>
#include <utility>
namespace blender::ed::sculpt_paint::undo {
struct ColorMasterSnapshotV04 {
 struct Item {
  MultiresColorGrid grid{};
  ~Item(){MEM_SAFE_DELETE(grid.colors);}
 };
 struct Entry {
  int grid_index=-1;
  std::unique_ptr<Item> item;
 };
 int level=0,corners=0;
 std::string attribute;
 Array<int> grid_to_saved;
 Vector<Entry> saved;
 size_t memory_bytes() const {
  size_t bytes=sizeof(*this)+grid_to_saved.as_span().size_in_bytes()+saved.as_span().size_in_bytes();
  for(const Entry &entry:saved){bytes+=sizeof(Item)+size_t(entry.item->grid.totcolor)*sizeof(float[4]);}
  return bytes;
 }
 bool capture(MultiresModifierData &mmd,const int *indices,int count){
  if(!mmd.sculpt_color_grids||mmd.sculpt_color_level<1||mmd.sculpt_color_level>13)return false;
  if(!level){
   level=mmd.sculpt_color_level;corners=mmd.sculpt_color_grids_num;attribute=mmd.sculpt_color_attribute;
   grid_to_saved.reinitialize(corners);grid_to_saved.fill(-1);
  }
  if(level!=mmd.sculpt_color_level||corners!=mmd.sculpt_color_grids_num||attribute!=mmd.sculpt_color_attribute)return false;
  for(int n=0;n<count;n++){
   const int g=indices[n];if(g<0||g>=corners)return false;
   if(grid_to_saved[g]!=-1)continue;
   auto item=std::make_unique<Item>();item->grid=mmd.sculpt_color_grids[g];item->grid.colors=nullptr;
   const auto &live=mmd.sculpt_color_grids[g];
   if(live.colors){
    const int side=bke::subdiv::grid_size_from_level(level);
    if(live.level!=level||live.totcolor!=side*side)return false;
    item->grid.colors=MEM_new_array_uninitialized<float[4]>(live.totcolor,"L0 Color Undo V04");
    std::memcpy(item->grid.colors,live.colors,size_t(live.totcolor)*sizeof(float[4]));
   }
   grid_to_saved[g]=saved.size();
   saved.append({g,std::move(item)});
  }
  return true;
 }
 bool exchange(MultiresModifierData &mmd){
  if(attribute!=mmd.sculpt_color_attribute){
   if(!bke::multires_layers::polypaint_attribute_activate(mmd,attribute.c_str()))return false;
  }
  if(!mmd.sculpt_color_grids&&!bke::multires_layers::polypaint_ensure(mmd,level,corners))return false;
  if(mmd.sculpt_color_level!=level||mmd.sculpt_color_grids_num!=corners)return false;
  for(Entry &entry:saved)std::swap(mmd.sculpt_color_grids[entry.grid_index],entry.item->grid);
  return true;
 }
};
}
