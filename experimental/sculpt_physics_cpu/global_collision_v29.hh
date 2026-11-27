/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physics_core.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {
/* Conservative whole-mesh swept broad phase. For nonadjacent triangles
 * whose initial AABBs are separated, bound the entire linear trajectory
 * to keep their swept AABBs separated. Existing overlapping AABBs are
 * explicitly counted as unresolved: this is NOT exact triangle CCD.
 * Rebuilds the index each call; intended for opt-in safety experiments. */
struct GlobalCollisionV29Result {
  double scale=1.0;
  std::size_t tested_pairs=0,limited_pairs=0,unresolved_pairs=0;
  bool budget_exhausted=false;
};
namespace collision_v29 {
struct Box {
  std::array<double,3> lo{{INFINITY,INFINITY,INFINITY}};
  std::array<double,3> hi{{-INFINITY,-INFINITY,-INFINITY}};
};
inline void add(Box &b,Vec3 p)
{
  const double a[3]={p.x,p.y,p.z};
  for(int j=0;j<3;++j){
    if(!std::isfinite(a[j]))throw std::invalid_argument("Nonfinite collision coordinate");
    b.lo[j]=std::min(b.lo[j],a[j]);b.hi[j]=std::max(b.hi[j],a[j]);
  }
}
inline Box merge(Box a,const Box &b)
{
  for(int j=0;j<3;++j){a.lo[j]=std::min(a.lo[j],b.lo[j]);a.hi[j]=std::max(a.hi[j],b.hi[j]);}
  return a;
}
inline bool overlap(const Box &a,const Box &b,double eps)
{
  for(int j=0;j<3;++j)if(a.hi[j]+eps<b.lo[j] || b.hi[j]+eps<a.lo[j])return false;
  return true;
}
// A fixed separating axis certifies a positive gap throughout the linear
// motion interval. Conservative, not exact feature CCD.
inline Vec3 axis_cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline double axis_dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline double separating_scale(const std::array<Vec3,3>& a,const std::array<Vec3,3>& b,
                               const std::array<Vec3,3>& ma,const std::array<Vec3,3>& mb,
                               double margin,double limit)
{
 const std::array<Vec3,3> ea{a[1]-a[0],a[2]-a[1],a[0]-a[2]},eb{b[1]-b[0],b[2]-b[1],b[0]-b[2]};
 const Vec3 na=axis_cross(ea[0],ea[1]),nb=axis_cross(eb[0],eb[1]);
 double best=0.;
 const auto test=[&](Vec3 axis){
  const double n=std::hypot(axis.x,axis.y,axis.z);
  if(!std::isfinite(n)||n<=1e-24)return;
  axis=axis/n;
  for(double sign:{-1.,1.}){
   double safe=limit;const auto u=axis*sign;
   for(int i=0;i<3;++i)for(int j=0;j<3;++j){
    const double gap=axis_dot(b[j]-a[i],u)-margin;
    const double speed=axis_dot(mb[j]-ma[i],u);
    if(!std::isfinite(gap)||!std::isfinite(speed)||gap<=0.){safe=0.;continue;}
    if(speed<0. && gap+limit*speed<=0.)safe=std::min(safe,.98*gap/-speed);
   }
   best=std::max(best,safe);
  }
 };
 test(na);test(nb);
 for(auto u:ea){test(axis_cross(na,u));for(auto v:eb)test(axis_cross(u,v));}
 for(auto v:eb)test(axis_cross(nb,v));
 return best;
}
struct Item {Box initial,swept;bool moving=false;};
struct Node {Box box;std::size_t start=0,end=0,left=0,right=0;};
}
template<class TriangleAt,class PositionAt,class MotionAt>
GlobalCollisionV29Result guard_global_collision_v29(std::size_t triangle_count,
    TriangleAt triangle_at,PositionAt position_at,MotionAt motion_at,
    double margin=0.0,std::size_t pair_budget=200000,bool strict=false)
{
  using namespace collision_v29;
  if(!std::isfinite(margin) || margin<0 || pair_budget==0)
    throw std::invalid_argument("Invalid collision guard parameters");
  GlobalCollisionV29Result out;
  if(triangle_count==0)return out;
  std::vector<Item> items(triangle_count);
  const auto bounds=[&](std::size_t id,double scale){
    Box box;
    for(auto v:triangle_at(id)){
      const Vec3 p=position_at(v);
      add(box,p);add(box,p+motion_at(v)*scale);
    }
    return box;
  };
  bool any=false;
  for(std::size_t i=0;i<triangle_count;++i){
    auto &item=items[i];item.initial=bounds(i,0);
    for(auto v:triangle_at(i)){const Vec3 m=motion_at(v);item.moving|=m.x!=0||m.y!=0||m.z!=0;}
    item.swept=item.moving?bounds(i,1):item.initial;any|=item.moving;
  }
  if(!any)return out;
  std::vector<std::size_t> order(triangle_count);
  std::iota(order.begin(),order.end(),0);
  std::vector<Node> tree;
  const auto build=[&](auto &&self,std::size_t start,std::size_t end)->std::size_t{
    const auto id=tree.size();tree.emplace_back();
    Box box;
    for(std::size_t i=start;i<end;++i)box=merge(box,items[order[i]].swept);
    tree[id].box=box;tree[id].start=start;tree[id].end=end;
    if(end-start<=8)return id;
    int axis=0;
    for(int j=1;j<3;++j)if(box.hi[j]-box.lo[j]>box.hi[axis]-box.lo[axis])axis=j;
    const auto mid=start+(end-start)/2;
    std::nth_element(order.begin()+start,order.begin()+mid,order.begin()+end,[&](auto a,auto b){
      const auto &x=items[a].swept,&y=items[b].swept;
      return .5*x.lo[axis]+.5*x.hi[axis]<.5*y.lo[axis]+.5*y.hi[axis];
    });
    const auto left=self(self,start,mid),right=self(self,mid,end);
    tree[id].left=left;tree[id].right=right;return id;
  };
  build(build,0,triangle_count);
  for(std::size_t i=0;i<triangle_count;++i)if(items[i].moving){
    std::vector<std::size_t> stack{0};
    while(!stack.empty()){
      const auto id=stack.back();stack.pop_back();
      const auto &node=tree[id];
      if(!overlap(items[i].swept,node.box,margin))continue;
      if(node.end-node.start>8){stack.push_back(node.left);stack.push_back(node.right);continue;}
      for(auto k=node.start;k<node.end;++k){
        const auto j=order[k];
        if(i==j || (items[j].moving && j<i) || !overlap(items[i].swept,items[j].swept,margin))continue;
        bool adjacent=false;
        for(auto a:triangle_at(i))for(auto b:triangle_at(j))adjacent|=a==b;
        if(adjacent)continue;
        if(++out.tested_pairs>pair_budget){out.scale=0;out.budget_exhausted=true;return out;}
        if(overlap(items[i].initial,items[j].initial,margin)){
          if(!strict){++out.unresolved_pairs;continue;}
          std::array<Vec3,3> a{},b{},ma{},mb{};
          const auto ta=triangle_at(i),tb=triangle_at(j);
          for(int k=0;k<3;++k){a[k]=position_at(ta[k]);b[k]=position_at(tb[k]);
            ma[k]=motion_at(ta[k]);mb[k]=motion_at(tb[k]);}
          const double safe=separating_scale(a,b,ma,mb,margin,out.scale);
          if(safe==0.){++out.unresolved_pairs;out.scale=0.;return out;}
          if(safe<out.scale){out.scale=safe;++out.limited_pairs;}
          continue;
        }
        if(!overlap(bounds(i,out.scale),bounds(j,out.scale),margin))continue;
        double lo=0,hi=out.scale;
        for(int step=0;step<32;++step){
          const double mid=.5*(lo+hi);
          if(overlap(bounds(i,mid),bounds(j,mid),margin))hi=mid;else lo=mid;
        }
        out.scale=lo*.98;++out.limited_pairs;
        if(out.scale==0)return out;
      }
    }
  }
  return out;
}
} // namespace sculpt_physics
