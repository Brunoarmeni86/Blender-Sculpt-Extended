/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "sculpt_polypaint_v04_api.hh"
#include <map>
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
 int level=0,corners=0;
 std::string attribute;
 std::map<int,std::unique_ptr<Item>> saved;
 size_t memory_bytes() const {
  size_t bytes=sizeof(*this);
  for(const auto &entry:saved){bytes+=sizeof(Item)+sizeof(int)+64+size_t(entry.second->grid.totcolor)*sizeof(float[4]);}
  return bytes;
 }
 bool capture(MultiresModifierData &mmd,const int *indices,int count){
  if(!mmd.sculpt_color_grids||mmd.sculpt_color_level<1||mmd.sculpt_color_level>13)return false;
  if(!level){level=mmd.sculpt_color_level;corners=mmd.sculpt_color_grids_num;attribute=mmd.sculpt_color_attribute;}
  if(level!=mmd.sculpt_color_level||corners!=mmd.sculpt_color_grids_num||attribute!=mmd.sculpt_color_attribute)return false;
  for(int n=0;n<count;n++){
   const int g=indices[n];if(g<0||g>=corners)return false;
   if(saved.find(g)!=saved.end())continue;
   auto item=std::make_unique<Item>();item->grid=mmd.sculpt_color_grids[g];item->grid.colors=nullptr;
   const auto &live=mmd.sculpt_color_grids[g];
   if(live.colors){
    const int side=bke::subdiv::grid_size_from_level(level);
    if(live.level!=level||live.totcolor!=side*side)return false;
    item->grid.colors=MEM_new_array_uninitialized<float[4]>(live.totcolor,"L0 Color Undo V04");
    std::memcpy(item->grid.colors,live.colors,size_t(live.totcolor)*sizeof(float[4]));
   }
   saved.emplace(g,std::move(item));
  }
  return true;
 }
 bool exchange(MultiresModifierData &mmd){
  if(attribute!=mmd.sculpt_color_attribute){
   if(!bke::multires_layers::polypaint_attribute_activate(mmd,attribute.c_str()))return false;
  }
  if(!mmd.sculpt_color_grids&&!bke::multires_layers::polypaint_ensure(mmd,level,corners))return false;
  if(mmd.sculpt_color_level!=level||mmd.sculpt_color_grids_num!=corners)return false;
  for(auto &entry:saved)std::swap(mmd.sculpt_color_grids[entry.first],entry.second->grid);
  return true;
 }
};
}
