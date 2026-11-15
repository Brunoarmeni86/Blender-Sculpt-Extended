/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "sculpt_polypaint_v04_api.hh"
#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
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
  struct Pixel { int index; float color[4]; };
  BitVector<> captured;
  Vector<Pixel> pixels;
  bool full_exchange=false;
 };
 int level=0,corners=0;
 std::string attribute;
 Array<int> grid_to_saved;
 Vector<Entry> saved;
 size_t memory_bytes() const {
  size_t bytes=sizeof(*this)+grid_to_saved.as_span().size_in_bytes()+saved.as_span().size_in_bytes();
  for(const Entry &entry:saved){
   bytes+=sizeof(Item)+size_t(entry.item->grid.totcolor)*sizeof(float[4]);
   bytes+=(entry.captured.size()+7)/8+entry.pixels.as_span().size_in_bytes();
  }
  return bytes;
 }
 bool ensure_header(MultiresModifierData &mmd){
  if(!mmd.sculpt_color_grids||mmd.sculpt_color_level<1||mmd.sculpt_color_level>13)return false;
  if(!level){
   level=mmd.sculpt_color_level;corners=mmd.sculpt_color_grids_num;attribute=mmd.sculpt_color_attribute;
   grid_to_saved.reinitialize(corners);grid_to_saved.fill(-1);
  }
  if(level!=mmd.sculpt_color_level||corners!=mmd.sculpt_color_grids_num||attribute!=mmd.sculpt_color_attribute)return false;
  return true;
 }
 bool capture_region(MultiresModifierData &mmd,const int g,const int x0,const int y0,
                     const int width,const int height){
  if(!ensure_header(mmd)||g<0||g>=corners||width<1||height<1)return false;
  const int side=bke::subdiv::grid_size_from_level(level);
  if(x0<0||y0<0||x0+width>side||y0+height>side)return false;
  int entry_index=grid_to_saved[g];
  if(entry_index==-1){
   Entry entry;entry.grid_index=g;entry.item=std::make_unique<Item>();
   entry.item->grid=mmd.sculpt_color_grids[g];entry.item->grid.colors=nullptr;
   const auto &live=mmd.sculpt_color_grids[g];
   if(!live.colors){entry.full_exchange=true;}
   else{
    if(live.level!=level||live.totcolor!=side*side)return false;
    entry.captured.resize(live.totcolor,false);
   }
   entry_index=saved.size();grid_to_saved[g]=entry_index;saved.append(std::move(entry));
  }
  Entry &entry=saved[entry_index];
  if(entry.full_exchange)return true;
  auto &live=mmd.sculpt_color_grids[g];
  if(!live.colors||live.level!=level||live.totcolor!=side*side)return false;
  for(int y=y0;y<y0+height;y++)for(int x=x0;x<x0+width;x++){
   const int i=y*side+x;if(entry.captured[i])continue;
   Entry::Pixel pixel;pixel.index=i;std::memcpy(pixel.color,live.colors[i],sizeof(pixel.color));
   entry.pixels.append(pixel);entry.captured[i].set();
  }
  return true;
 }
 bool exchange(MultiresModifierData &mmd){
  if(attribute!=mmd.sculpt_color_attribute){
   if(!bke::multires_layers::polypaint_attribute_activate(mmd,attribute.c_str()))return false;
  }
  if(!mmd.sculpt_color_grids&&!bke::multires_layers::polypaint_ensure(mmd,level,corners))return false;
  if(mmd.sculpt_color_level!=level||mmd.sculpt_color_grids_num!=corners)return false;
  for(Entry &entry:saved){
   auto &live=mmd.sculpt_color_grids[entry.grid_index];
   if(entry.full_exchange){std::swap(live,entry.item->grid);continue;}
   if(!live.colors||live.level!=level)return false;
   for(Entry::Pixel &pixel:entry.pixels)for(int k=0;k<4;k++)std::swap(live.colors[pixel.index][k],pixel.color[k]);
  }
  return true;
 }
};
}
