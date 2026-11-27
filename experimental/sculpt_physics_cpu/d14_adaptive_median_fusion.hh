/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "d12_brush_common.hh"
#include "artistic_paired_flux.hh"
#include <array>
#include <utility>
#include <limits>

namespace sculpt_physics::dedicated_d14::median_fusion {
namespace d=dedicated_d12;
using Edge=std::array<std::size_t,2>;
struct Settings {
 double radius=1.;
 Vec3 axis{0,0,1};
 double mobility=.65; // fraction of locally added height available as old-material allowance
 double old_limit=.025; // old material movement per dab, in radius units
 double relaxation=.65; // median-guided exchange response
};
struct Statistics {
 bool gpu=false;
 std::size_t links=0,old_donors=0;
 double transferred=0.,old_mobilized=0.,axial_balance=0.;
};
using WeightedOffset=std::pair<double,double>;
inline double median(std::vector<WeightedOffset>& a){
 if(a.empty())return 0.;
 std::sort(a.begin(),a.end(),[](auto x,auto y){return x.first<y.first;});
 double sum=0.;for(auto x:a)sum+=x.second;
 double cumulative=0.;
 for(auto x:a){cumulative+=x.second;if(cumulative>=.5*sum)return x.first;}
 return a.back().first;
}
// Host provides unique incident edges and actual lumped areas, already
// filtered to writable tool support. Only vertices with positive deposition
// participate in this V1 adapter. The live surface is a relief proxy, not a
// persistent stroke label or a measurement of physically deposited mass.
// All updates are staged and applied only after capacity/balance checks.
inline bool apply(const Settings& k,const std::vector<PhysicalBrushIntentSample>& s,
 const std::vector<Edge>& edges,const std::vector<double>& area,
 const std::vector<double>& headroom,std::vector<Vec3>& motion,
 PairedFluxBackend* gpu=nullptr,Statistics* stats=nullptr){
 if(stats)*stats={};
 const auto n=s.size();
 if(area.size()!=n||headroom.size()!=n||motion.size()!=n||
    !d::finite(k.axis)||std::abs(d::norm2(k.axis)-1.)>1e-5||
    !std::isfinite(k.radius)||k.radius<=0.||
    !std::isfinite(k.mobility)||k.mobility<0.||k.mobility>1.||
    !std::isfinite(k.old_limit)||k.old_limit<0.||k.old_limit>.1||
    !std::isfinite(k.relaxation)||k.relaxation<0.||k.relaxation>1.)return false;
 std::vector<double> deposit(n),degree(n),near_deposit(n),overlap(n),target(n);
 std::vector<std::vector<WeightedOffset>> offsets(n);
 std::vector<unsigned char> eligible(n);
 for(std::size_t i=0;i<n;++i){
  if(!std::isfinite(area[i])||area[i]<0.||!std::isfinite(headroom[i])||
     headroom[i]<0.||!d::finite(motion[i]))return false;
  if(!d::valid_sample(s[i])||area[i]<=1e-24||headroom[i]<=0.)continue;
  if(d::dot(d::unit_normal(s[i].surface_normal),k.axis)<=.2)continue;
  const double h=d::dot(motion[i],k.axis);
  if(h<=1e-15)continue;
  eligible[i]=1;deposit[i]=h;near_deposit[i]=h;
 }
 struct Contact {std::size_t a,b;double gap,weight;};
 std::vector<Contact> contact;contact.reserve(edges.size());
 for(auto e:edges){
  const auto a=e[0],b=e[1];
  if(a>=n||b>=n)return false;
  if(a==b||!eligible[a]||!eligible[b])continue;
  const Vec3 old=s[a].position-s[b].position;
  const double length=d::norm(old);
  if(length<=1e-12*k.radius||length>.5*k.radius)continue;
  const auto na=d::unit_normal(s[a].surface_normal),nb=d::unit_normal(s[b].surface_normal);
  if(d::dot(na,nb)<.35)continue;
  const auto normal=d::unit(na+nb);
  const double alignment=d::dot(normal,k.axis);
  if(alignment<=.2)continue;
  // Tangent-plane detrending: a locally inclined plane has zero residual.
  const double old_gap=d::dot(old,normal)/alignment;
  const double gap=d::dot(old+motion[a]-motion[b],normal)/alignment;
  if(!std::isfinite(old_gap)||!std::isfinite(gap))return false;
  // Gate by relief already present, and its slope relative to edge length.
  // No fusion on an initially flat/sloping planar surface. This avoids
  // erasing the individual tool profile on first contact.
  const double t=std::clamp(std::abs(old_gap)/(.15*length),0.,1.);
  const double gate=t*t*(3.-2.*t);
  if(gate<1e-5)continue;
  const double weight=gate*std::min(s[a].envelope,s[b].envelope);
  if(weight<=0.)continue;
  offsets[a].push_back({-gap,weight*area[b]});
  offsets[b].push_back({gap,weight*area[a]});
  degree[a]+=weight;degree[b]+=weight;
  near_deposit[a]=std::max(near_deposit[a],deposit[b]);
  near_deposit[b]=std::max(near_deposit[b],deposit[a]);
  overlap[a]=std::max(overlap[a],weight);
  overlap[b]=std::max(overlap[b],weight);
  contact.push_back({a,b,gap,weight});
 }
 if(contact.empty())return true;
 std::vector<double> give(n),take(n);
 for(std::size_t i=0;i<n;++i)if(eligible[i]&&degree[i]>0.){
  // Include a modest self-anchor in the median so that a single weak
  // neighbor cannot redefine the entire tool target.
  double total=0.;for(auto x:offsets[i])total+=x.second;
  if(!std::isfinite(total)||total<=0.)continue;
  offsets[i].push_back({0.,.25*total});
  target[i]=median(offsets[i]);
  const double old_budget=std::min(k.old_limit*k.radius,k.mobility*near_deposit[i])*overlap[i];
  const double receive_budget=std::min(.06*k.radius,1.5*near_deposit[i])*overlap[i];
  // headroom is a conservative magnitude budget after the raw proposal;
  // bound both signs to preserve the original first-touch cap.
  give[i]=std::min(headroom[i],deposit[i]+old_budget)*area[i];
  take[i]=std::min(headroom[i],receive_budget)*area[i];
 }
 std::vector<PairedFluxLink> links;links.reserve(contact.size());
 for(auto e:contact){
  auto a=e.a,b=e.b;double gap=e.gap;
  if(gap<0.){std::swap(a,b);gap=-gap;}
  if(gap<=0.||give[a]<=0.||take[b]<=0.)continue;
  const double median_drive=std::max(0.,target[b]-target[a]);
  // Limit proposal to the isolated-pair equilibrium (not a guarantee
  // against overshoot in arbitrary multi-edge geometry).
  const double harmonic=1./(1./area[a]+1./area[b]);
  const double q=k.relaxation*e.weight/std::max({1.,degree[a],degree[b]})*
      harmonic*std::min(gap,median_drive);
  if(!std::isfinite(q))return false;
  if(q>0.)links.push_back({a,b,q});
 }
 if(links.empty())return true;
 std::vector<double> change;double moved=0.;
 bool accelerated=paired_flux_dispatch(gpu,links,give,take,change,moved);
 // Backend's global proposal tolerance can be larger than a small
 // individual capacity. Enforce per-vertex motion and budget here.
 const auto validate=[&](){
  if(change.size()!=n||!std::isfinite(moved)||moved<0.)return false;
  double balance=0.,magnitude=0.;
  for(std::size_t i=0;i<n;++i){
   const double q=change[i];
   const double tol=1e-10*std::max({1e-30,give[i],take[i]});
   if(!std::isfinite(q)||q < -give[i]-tol||q>take[i]+tol)return false;
   if(q!=0.&&(!eligible[i]||area[i]<=0.||std::abs(q/area[i])>headroom[i]+tol/area[i]))return false;
   if(q!=0.&&!d::finite(motion[i]+k.axis*(q/area[i])))return false;
   balance+=q;magnitude+=std::abs(q);
  }
  return std::isfinite(balance)&&std::isfinite(magnitude)&&
      std::abs(balance)<=1e-9*std::max(1e-30,magnitude);
 };
 if(!validate()&&accelerated){
  paired_flux_reference(links,give,take,change,moved);accelerated=false;
 }
 if(!validate())return false;
 Statistics result;result.gpu=accelerated;result.links=links.size();result.transferred=moved;
 for(std::size_t i=0;i<n;++i)if(change[i]!=0.){
  const double h=change[i]/area[i];
  const double old=std::max(0.,-(deposit[i]+h))*area[i];
  if(old>0.){++result.old_donors;result.old_mobilized+=old;}
  result.axial_balance+=change[i];
  motion[i]+=k.axis*h;
 }
 if(stats)*stats=result;
 return true;
}
} // namespace sculpt_physics::dedicated_d14::median_fusion
