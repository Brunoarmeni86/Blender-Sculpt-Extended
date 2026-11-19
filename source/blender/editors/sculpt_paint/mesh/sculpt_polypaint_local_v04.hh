/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "sculpt_polypaint_blur_v01_math.hh"
#include <array>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <limits>
#include <functional>
namespace blender::ed::sculpt_paint::color::color_local_v04 {
using RGBA=std::array<float,4>;
struct UnitWeight { double operator()(int,int) const { return 1.0; } };
struct Result {
  std::unordered_map<int,RGBA> source;
  std::unordered_map<int,RGBA> writes;
  size_t groups=0;
};
struct PaintWrite { RGBA before; RGBA after; };
struct PaintResult {
  std::unordered_map<int,PaintWrite> writes;
  size_t samples=0;
  size_t groups=0;
};

/* Paint does not need neighbor rings. Fuse duplicate closure, source sampling
 * and transformation into one pass instead of constructing the generic
 * group/owner/candidate graph used by Blur and Smear. Shared CCG samples keep
 * the same conservative minimum influence and averaged source color. */
template<class Sample,class Duplicates,class Hidden,class Transform>
bool calculate_paint(const std::unordered_map<int,float> &influence,
                     Sample sample,
                     Duplicates duplicates,
                     Hidden hidden,
                     PaintResult &result,
                     size_t max_source,
                     Transform transform)
{
  result=PaintResult{};
  if(influence.size()>max_source)return false;
  const size_t reserve_hint=std::min(max_source,influence.size()+influence.size()/8+8);
  result.writes.max_load_factor(0.7f);
  result.writes.reserve(influence.size());
  std::unordered_set<int> visited;
  visited.max_load_factor(0.7f);
  /* Only seam samples enter this set. Reserving for every active interior
   * sample made broad strokes pay for a second dense hash table. */
  visited.reserve(std::min<size_t>(reserve_hint,influence.size()/16+64));
  std::vector<int> members;
  members.reserve(8);
  std::vector<RGBA> member_sources;
  member_sources.reserve(8);
  /* Paint has no neighbor reduction, so active sample order cannot affect the
   * result. Iterate the already-built influence table directly instead of
   * allocating and sorting 100k-300k indices for every symmetry pass. CCG
   * duplicate members are still sorted locally to preserve their exact mean. */
  for(const auto &active_entry:influence){
    if(active_entry.second<=0)continue;
    const int root=active_entry.first;
    members.clear();members.push_back(root);
    /* Interior samples cannot be duplicates of a seam sample. Process them
     * without visited-set lookups, duplicate closure, or member sorting. */
    const auto root_copies=duplicates(root);
    if(root_copies.empty()){
      if(result.samples>=max_source)return false;
      RGBA before;
      if(!sample(root,before))return false;
      for(const float value:before)if(!std::isfinite(value))return false;
      result.samples++;
      result.groups++;
      if(hidden(root))continue;
      RGBA out;
      if(!transform(root,before,active_entry.second,out))return false;
      for(const float value:out)if(!std::isfinite(value))return false;
      if(out!=before)result.writes.emplace(root,PaintWrite{before,out});
      continue;
    }
    if(visited.find(root)!=visited.end())continue;
    for(const int copy:root_copies){
      if(std::find(members.begin(),members.end(),copy)==members.end())members.push_back(copy);
    }
    for(size_t k=0;k<members.size();k++){
      if(k==0)continue;
      for(const int copy:duplicates(members[k])){
        if(std::find(members.begin(),members.end(),copy)==members.end())members.push_back(copy);
      }
    }
    std::sort(members.begin(),members.end());
    member_sources.clear();
    RGBA mean{};
    float factor=1.0f;
    bool protected_sample=false;
    for(const int i:members){
      if(!visited.insert(i).second)return false;
      if(result.samples>=max_source)return false;
      RGBA rgba;
      if(!sample(i,rgba))return false;
      for(int c=0;c<4;c++){
        if(!std::isfinite(rgba[c]))return false;
        mean[c]+=rgba[c];
      }
      result.samples++;
      member_sources.push_back(rgba);
      const auto found=influence.find(i);
      factor=std::min(factor,found==influence.end()?0.0f:found->second);
      protected_sample|=hidden(i);
    }
    result.groups++;
    if(factor<=0||protected_sample)continue;
    for(float &value:mean)value/=float(members.size());
    RGBA out;
    if(!transform(members.front(),mean,factor,out))return false;
    for(const float value:out)if(!std::isfinite(value))return false;
    for(size_t member_index=0;member_index<members.size();member_index++){
      const int i=members[member_index];
      const RGBA &before=member_sources[member_index];
      if(out!=before)result.writes.emplace(i,PaintWrite{before,out});
    }
  }
  return true;
}
/* Physical indices are stable CCG indices. Duplicate closure includes copies
 * outside the captured PBVH nodes, whose missing influence is exactly zero.
 * Source colors are immutable until the caller publishes all staged grids. */
template<class Sample,class Duplicates,class Neighbors,class Hidden,class Weight=UnitWeight>
bool calculate(const std::unordered_map<int,float> &influence,Sample sample,
               Duplicates duplicates,Neighbors neighbors,Hidden hidden,Result &result,
               Weight weight=Weight{},bool two_ring=false,
               size_t max_source=std::numeric_limits<size_t>::max(),
               std::function<bool(int,const RGBA&,float,RGBA&)> transform={})
{
  result=Result{};
  if (influence.size()>max_source) { return false; }
  const size_t reserve_hint=std::min(max_source,influence.size()+influence.size()/2+8);
  result.source.reserve(reserve_hint);
  struct Group {
    std::vector<int> members;
    RGBA mean{};
    float factor=1.0f;
    bool protected_sample=false;
  };
  std::vector<Group> groups;
  std::unordered_map<int,int> owner;
  groups.reserve(influence.size());
  owner.reserve(reserve_hint);
  auto get_group=[&](const int first)->int {
    auto found=owner.find(first);
    if (found!=owner.end()) { return found->second; }
    Group group;group.members.push_back(first);
    /* CCG duplicate fans are small. Close transitively, then sort so arithmetic
     * uses the same physical-index order as the full-mesh reference. */
    for (size_t k=0;k<group.members.size();k++) {
      for (const int copy:duplicates(group.members[k])) {
        if (std::find(group.members.begin(),group.members.end(),copy)==group.members.end()) {
          group.members.push_back(copy);
        }
      }
    }
    std::sort(group.members.begin(),group.members.end());
    for (const int i:group.members) {
      if (owner.find(i)!=owner.end()) { return -1; } /* Inconsistent duplicate relation. */
      if (result.source.size()>=max_source) { return -1; }
      RGBA rgba;
      if (!sample(i,rgba)) { return -1; }
      for (int c=0;c<4;c++) {
        if (!std::isfinite(rgba[c])) { return -1; }
        group.mean[c]+=rgba[c];
      }
      result.source.emplace(i,rgba);
      const auto f=influence.find(i);
      group.factor=std::min(group.factor,f==influence.end()?0.0f:f->second);
      group.protected_sample|=hidden(i);
    }
    for (int c=0;c<4;c++) { group.mean[c]/=float(group.members.size()); }
    const int id=int(groups.size());
    for (const int i:group.members) { owner.emplace(i,id); }
    groups.push_back(std::move(group));return id;
  };
  std::vector<int> active;
  active.reserve(influence.size());
  for (const auto &entry:influence) { if (entry.second>0) { active.push_back(entry.first); } }
  std::sort(active.begin(),active.end());
  std::vector<int> candidates;
  candidates.reserve(active.size());
  for (const int i:active) {
    const int id=get_group(i);if (id<0) { return false; }
    if (groups[id].members.front()==i && groups[id].factor>0 && !groups[id].protected_sample) {
      candidates.push_back(id);
    }
  }
  for (const int id:candidates) {
    const int root=groups[id].members.front();
    if(transform){
      RGBA out;
      if(!transform(root,groups[id].mean,groups[id].factor,out))return false;
      for(int c=0;c<4;c++)if(!std::isfinite(out[c]))return false;
      for(const int i:groups[id].members){
        const auto &before=result.source.at(i);
        if(out!=before)result.writes.emplace(i,out);
      }
      continue;
    }
    std::vector<int> adjacent;
    for (const int n:neighbors(root)) {
      const int other=get_group(n);if (other<0) { return false; }
      if (other!=id && !groups[other].protected_sample) { adjacent.push_back(other); }
    }
    if (two_ring) {
      const auto first_ring=adjacent;
      for (const int n:first_ring) {
        const int nroot=groups[n].members.front();
        for (const int physical:neighbors(nroot)) {
          const int other=get_group(physical);if (other<0) { return false; }
          if (other!=id && !groups[other].protected_sample) { adjacent.push_back(other); }
        }
      }
    }
    std::sort(adjacent.begin(),adjacent.end(),[&](int a,int b) {
      return groups[a].members.front()<groups[b].members.front();
    });
    adjacent.erase(std::unique(adjacent.begin(),adjacent.end()),adjacent.end());
    if (adjacent.empty()) { continue; }
    double sum[3]={0,0,0},total_weight=0;
    for (const int n:adjacent) {
      const double w=weight(root,groups[n].members.front());
      if (!std::isfinite(w) || w<0) { return false; }
      total_weight+=w;
      for (int c=0;c<3;c++) { sum[c]+=double(groups[n].mean[c])*w; }
    }
    if (total_weight<=0) { continue; }
    RGBA out=groups[id].mean;
    for (int c=0;c<3;c++) {
      const double value=double(out[c])*(1.0-double(groups[id].factor))+
          sum[c]/total_weight*double(groups[id].factor);
      out[c]=float(value);
      if (!std::isfinite(value) || !std::isfinite(out[c])) { return false; }
    }
    for (const int i:groups[id].members) {
      const RGBA &before=result.source.at(i);
      if (out[0]!=before[0] || out[1]!=before[1] || out[2]!=before[2]) {
        RGBA value=out;value[3]=before[3];result.writes.emplace(i,value);
      }
    }
  }
  result.groups=groups.size();return true;
}
}
