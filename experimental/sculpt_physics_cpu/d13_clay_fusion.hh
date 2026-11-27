/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "d12_clay_engine.hh"
#include "artistic_paired_flux.hh"
#include <array>
#include <limits>
#include <queue>
#include <functional>
namespace sculpt_physics::dedicated_d13::clay_fusion {
namespace d=dedicated_d12;
using Edge=std::array<std::size_t,2>;
// Contact topology and actual lumped triangle areas supplied by native host.
// Retain one connected, oriented eligible component seeded at the live hit.
inline std::vector<unsigned char> contact(const std::vector<PhysicalBrushIntentSample>& s,
 const std::vector<Edge>& edges,Vec3 center,Vec3 axis,double radius,double travel)
{
 const auto n=s.size();std::vector<unsigned char> ok(n),selected(n);
 std::vector<std::vector<std::size_t>> adj(n);
 if(!d::finite(center)||!d::finite(axis)||!std::isfinite(radius)||radius<=0||
    !std::isfinite(travel)||travel<0)return selected;
 axis=d::unit(axis);
 if(d::norm2(axis)<.5)return selected;
 std::size_t seed=n;double best=radius*radius;
 for(std::size_t i=0;i<n;++i){
  if(!d::valid_sample(s[i]))continue;
  const auto q=s[i].position-center;
  if(d::dot(d::unit_normal(s[i].surface_normal),axis)<=.05||
     std::abs(d::dot(q,axis))>.55*radius||
     d::norm2(q)>(2.*radius+travel)*(2.*radius+travel))continue;
  ok[i]=1;const double r=d::norm2(q);
  if(r<best){best=r;seed=i;}
 }
 if(seed==n)return selected;
 for(auto e:edges){
  const auto a=e[0],b=e[1];
  if(a>=n||b>=n)throw std::out_of_range("D13 contact edge");
  if(a==b||!ok[a]||!ok[b])continue;
  if(d::dot(d::unit_normal(s[a].surface_normal),d::unit_normal(s[b].surface_normal))<.2)continue;
  adj[a].push_back(b);adj[b].push_back(a);
 }
 // Bound intrinsic travel, not only Euclidean proximity. Avoid wrapping
 // around a thin fold into the opposite sheet through a long connected path.
 const double reach=1.5*radius+std::min(travel,1.5*radius);
 using Entry=std::pair<double,std::size_t>;
 std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
 std::vector<double> distance(n,std::numeric_limits<double>::infinity());
 distance[seed]=0.;queue.push({0.,seed});
 while(!queue.empty()){
  const auto [length,i]=queue.top();queue.pop();
  if(length!=distance[i])continue;
  selected[i]=1;
  for(auto j:adj[i]){
   const double candidate=length+d::norm(s[i].position-s[j].position);
   if(candidate<=reach&&candidate<distance[j]){
    distance[j]=candidate;queue.push({candidate,j});
   }
  }
 }
 return selected;
}
// Redistribute only NEW positive deposition. No implicit smooth of old mesh.
// Pair exchange conserves area-weighted axial displacement before writer;
// receiver headroom supplied by caller includes first-touch geometric cap.
inline bool redistribute(const std::vector<PhysicalBrushIntentSample>& samples,
 const std::vector<Edge>& edges,const std::vector<double>& area,
 const std::vector<double>& headroom,Vec3 axis,double radius,
 std::vector<Vec3>& motion,PairedFluxBackend* gpu=nullptr,bool* used=nullptr)
{
 if(used)*used=false;
 const auto n=samples.size();
 if(area.size()!=n||headroom.size()!=n||motion.size()!=n||
    !std::isfinite(radius)||radius<=0)return false;
 axis=d::unit(axis);if(d::norm2(axis)<.5)return false;
 std::vector<double> give(n),take(n),height(n),degree(n);
 for(std::size_t i=0;i<n;++i){
  if(!d::finite(motion[i])||!std::isfinite(area[i])||area[i]<0||
     !std::isfinite(headroom[i])||headroom[i]<0)return false;
  if(!d::valid_sample(samples[i])||area[i]<=1e-24)continue;
  const double deposit=std::max(0.,d::dot(motion[i],axis));
  // Do not widen support into points rejected by the original tool law.
  if(deposit<=1e-15)continue;
  give[i]=.45*deposit*area[i];
  take[i]=std::min({headroom[i],.75*deposit,.03*radius})*area[i];
  height[i]=d::dot(samples[i].position+motion[i],axis);
 }
 const auto eligible=[&](Edge e){
  return e[0]!=e[1]&&(give[e[0]]>0||take[e[0]]>0)&&(give[e[1]]>0||take[e[1]]>0)&&
   d::norm2(samples[e[0]].position-samples[e[1]].position)<.25*radius*radius&&
   d::dot(d::unit_normal(samples[e[0]].surface_normal),d::unit_normal(samples[e[1]].surface_normal))>.5;
 };
 for(auto e:edges){if(e[0]>=n||e[1]>=n)return false;if(eligible(e)){degree[e[0]]++;degree[e[1]]++;}}
 std::vector<PairedFluxLink> links;
 for(auto e:edges)if(eligible(e)){
  auto a=e[0],b=e[1];if(height[b]>height[a])std::swap(a,b);
  if(give[a]<=0||take[b]<=0)continue;
  const double q=.25*(height[a]-height[b])*std::min(area[a],area[b])/
      std::max({1.,degree[a],degree[b]});
  if(q>0)links.push_back({a,b,q});
 }
 if(links.empty())return true;
 std::vector<double> delta;double moved=0.;
 const bool accelerated=paired_flux_dispatch(gpu,links,give,take,delta,moved);
 if(used)*used=accelerated;
 for(std::size_t i=0;i<n;++i)if(delta[i]!=0.)motion[i]+=axis*(delta[i]/area[i]);
 return true;
}
} // namespace sculpt_physics::dedicated_d13::clay_fusion
