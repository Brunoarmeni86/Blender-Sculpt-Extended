/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "d14_adaptive_median_fusion.hh"
#include "d14_continuous_clay.hh"
#include <queue>
namespace sculpt_physics::dedicated_d14::continuous_fusion {
namespace d=dedicated_d12;
namespace m=median_fusion;
namespace c=continuous_clay;
using Edge=m::Edge;
struct Stats {
 bool gpu=false;
 std::size_t links=0,collar_writes=0,old_donors=0;
 double transferred=0.,balance=0.,old_mobilized=0.;
};
// Bounded material activity propagation and conservative pair exchange.
// Old relief and excess motion gradients drive the transfer. No generic
// whole-mesh Smooth, and no movement without new deposition.
inline bool apply(const m::Settings& k,const std::vector<PhysicalBrushIntentSample>& s,
 const std::vector<Edge>& edges,const std::vector<double>& area,
 const std::vector<double>& headroom,std::vector<Vec3>& motion,
 PairedFluxBackend* gpu=nullptr,Stats* stats=nullptr){
 if(stats)*stats={};
 const auto n=s.size();
 if(area.size()!=n||headroom.size()!=n||motion.size()!=n||
    !d::finite(k.axis)||std::abs(d::norm2(k.axis)-1.)>1e-5||
    !std::isfinite(k.radius)||k.radius<=0.||
    !std::isfinite(k.mobility)||k.mobility<0.||k.mobility>1.||
    !std::isfinite(k.old_limit)||k.old_limit<0.||k.old_limit>.1||
    !std::isfinite(k.relaxation)||k.relaxation<0.||k.relaxation>1.)return false;
 std::vector<double> raw(n),activity(n),weight(n),offset(n),degree(n);
 std::vector<Vec3> normals(n);
 double peak=0.;
 for(std::size_t i=0;i<n;++i){
  if(!std::isfinite(area[i])||area[i]<0.||!std::isfinite(headroom[i])||
     headroom[i]<0.||!d::finite(motion[i]))return false;
  raw[i]=d::dot(motion[i],k.axis);
  if(!d::valid_sample(s[i])||area[i]<=1e-24||headroom[i]<=0.)continue;
  normals[i]=d::unit_normal(s[i].surface_normal);
  weight[i]=s[i].envelope*c::fade((d::dot(normals[i],k.axis)-.05)/.35);
  activity[i]=weight[i]>0.?std::max(0.,raw[i]):0.;
  peak=std::max(peak,activity[i]);
 }
 struct Pair {std::size_t a,b;double length,weight,relief,gate;};
 std::vector<Pair> pairs;pairs.reserve(edges.size());
 std::vector<std::vector<std::pair<std::size_t,double>>> adj(n);
 for(auto e:edges){
  const auto a=e[0],b=e[1];
  if(a>=n||b>=n)return false;
  if(a==b||weight[a]<=0.||weight[b]<=0.)continue;
  const auto p=s[a].position-s[b].position;
  const double len=d::norm(p);
  if(len<=1e-12*k.radius||len>.5*k.radius)continue;
  const double angle=d::dot(normals[a],normals[b]);
  const auto normal=d::unit(normals[a]+normals[b]);
  const double align=d::dot(normal,k.axis);
  if(angle<=.05||align<=.15)continue;
  const double bend=c::fade((1.-angle)/.55);
  const double residual=d::dot(p,normal)/align;
  const double relief=std::clamp((1.-bend)*residual+bend*d::dot(p,k.axis),
                                -.12*k.radius,.12*k.radius);
  const double gate=c::fade(std::abs(relief)/(.15*len));
  const double w=std::min(weight[a],weight[b])*c::fade((angle-.05)/.35);
  if(w<=0.)continue;
  pairs.push_back({a,b,len,w,relief,gate});
  degree[a]+=w;degree[b]+=w;
  const double atten=std::exp(-len/(.085*k.radius));
  adj[a].push_back({b,atten});adj[b].push_back({a,atten});
 }
 if(peak<=1e-30||pairs.empty())return true;
 // Intrinsic distance (exponential attenuation), not a fixed ring count.
 // The soft cutoff is relative to the strongest current deposition.
 std::priority_queue<std::pair<double,std::size_t>> queue;
 for(std::size_t i=0;i<n;++i)if(activity[i]>0.)queue.push({activity[i],i});
 while(!queue.empty()){
  const auto [v,i]=queue.top();queue.pop();
  if(v!=activity[i])continue;
  for(auto [j,atten]:adj[i]){
   const double q=v*atten;
   if(q>peak*.005&&q>activity[j]){
    activity[j]=q;queue.push({q,j});
   }
  }
 }
 for(auto& a:activity)a*=c::fade((a/peak-.005)/.025);
 Stats out;
 std::vector<double> old_budget(n),receive(n),give(n),take(n);
 for(std::size_t i=0;i<n;++i){
  old_budget[i]=std::min(k.old_limit*k.radius,k.mobility*activity[i])*weight[i];
  receive[i]=std::min({headroom[i],.08*k.radius,1.8*activity[i]*weight[i]});
 }
 // Three conservative exchanges with a cumulative net correction budget.
 for(int pass=0;pass<3;++pass){
  for(std::size_t i=0;i<n;++i){
   const double outgoing=std::min(headroom[i],std::max(0.,raw[i])+old_budget[i]);
   give[i]=std::max(0.,outgoing+offset[i])*area[i];
   take[i]=std::max(0.,receive[i]-offset[i])*area[i];
  }
  std::vector<PairedFluxLink> links;links.reserve(pairs.size());
  for(auto e:pairs){
   if(activity[e.a]<=0.||activity[e.b]<=0.)continue;
   auto a=e.a,b=e.b;
   const double dh=raw[a]+offset[a]-raw[b]-offset[b];
   const double excess=std::copysign(std::max(0.,std::abs(dh)-.35*e.length),dh);
   double drive=excess+.60*e.gate*(e.relief+dh);
   if(drive<0.){std::swap(a,b);drive=-drive;}
   if(drive<=1e-15||give[a]<=0.||take[b]<=0.)continue;
   const double harmonic=1./(1./area[a]+1./area[b]);
   const double q=k.relaxation*e.weight/std::max({1.,degree[a],degree[b]})*harmonic*drive;
   if(!std::isfinite(q))return false;
   if(q>0.)links.push_back({a,b,q});
  }
  if(links.empty())break;
  std::vector<double> delta;double moved=0.;
  bool used=paired_flux_dispatch(gpu,links,give,take,delta,moved);
  const auto valid=[&](){
   if(delta.size()!=n||!std::isfinite(moved)||moved<0.)return false;
   double balance=0.,magnitude=0.;
   for(std::size_t i=0;i<n;++i){
    const double v=delta[i],tol=1e-9*std::max({1e-30,give[i],take[i]});
    if(!std::isfinite(v)||v < -give[i]-tol||v>take[i]+tol)return false;
    if(v!=0.&&(weight[i]<=0.||activity[i]<=0.||area[i]<=1e-24||
       std::abs(offset[i]+v/area[i])>headroom[i]+tol/area[i]))return false;
    balance+=v;magnitude+=std::abs(v);
   }
   return std::isfinite(balance)&&std::isfinite(magnitude)&&
       std::abs(balance)<=1e-8*std::max(1e-30,magnitude);
  };
  if(!valid()&&used){paired_flux_reference(links,give,take,delta,moved);used=false;}
  if(!valid())return false;
  if(moved<=1e-30)break;
  out.gpu=out.gpu||used;out.links+=links.size();out.transferred+=moved;
  for(std::size_t i=0;i<n;++i)if(delta[i]!=0.)offset[i]+=delta[i]/area[i];
 }
 double magnitude=0.;
 for(std::size_t i=0;i<n;++i){
  if(!d::finite(motion[i]+k.axis*offset[i]))return false;
  out.balance+=area[i]*offset[i];magnitude+=std::abs(area[i]*offset[i]);
  if(raw[i]<=1e-15&&offset[i]>1e-15)++out.collar_writes;
  const double old=std::max(0.,-(raw[i]+offset[i]))*area[i];
  if(old>0.&&offset[i]<0.){++out.old_donors;out.old_mobilized+=old;}
 }
 if(!std::isfinite(out.balance)||!std::isfinite(magnitude)||
    std::abs(out.balance)>1e-8*std::max(1e-30,magnitude))return false;
 for(std::size_t i=0;i<n;++i)motion[i]+=k.axis*offset[i];
 if(stats)*stats=out;
 return true;
}
}
