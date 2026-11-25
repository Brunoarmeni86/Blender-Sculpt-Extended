/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physics_core.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

namespace sculpt_physics {
/* Qualitative surface emulation; not measured rheology or bulk simulation.
 * Native tool produces the complete footprint and signed target first. */
struct NativeMaterialV32 {
  double plasticity,cohesion,viscosity,stiffness,transport,anisotropy;
};
inline NativeMaterialV32 native_material_v32(std::string_view family)
{
  if(family=="Wax")   return {.85,.65,.55,.45,.12,.25};
  if(family=="Stone") return {.35,.85,.25,.95,.025,.25};
  if(family=="Wood")  return {.55,.8,.25,.75,.05,.85};
  if(family=="Flesh") return {.45,.65,.4,.35,.13,.15};
  if(family=="Mud")   return {.95,.20,.75,.1,.28,.45};
  if(family=="Fabric")return {.30,.85,.35,.5,.10,.8};
  if(family=="Foam")  return {.40,.35,.3,.2,.12,.05};
  return {.90,.55,.30,.30,.12,.10}; // Clay
}
inline double native_gain_v32(const NativeMaterialV32 &p,int action,double amount)
{
  // Stay near the native response; never substitute a depth or pressure slab.
  double gain=1.0-.12*p.stiffness-.08*p.viscosity+.08*p.plasticity;
  // Raw brush DNA action IDs. Actual direction still comes from native tool.
  if(action==2 || action==5 || action==7 || action==8)gain-=.08*p.stiffness;
  if(action==11 || action==13)gain-=.05*p.viscosity;
  return 1.0+std::clamp(amount,0.0,1.0)*(std::clamp(gain,.75,1.08)-1.0);
}
struct NativeTransportV32Result {double correction_scale=0,relative_budget_error=0;std::size_t links=0;};
/* Redistribution of existing goals only, using positive real projected
 * dual areas. Each actual mesh edge exchanges equal/opposite integrated
 * vector motion. No new target, no zero-target bridge, no invented mass.
 * A single cap preserves the pair budget and retains native target sign. */
inline NativeTransportV32Result redistribute_native_v32(
    const std::vector<Vec3> &positions,const std::vector<double> &area,
    const std::vector<std::array<std::size_t,2>> &edges,
    Vec3 direction,double radius,double amount,double anisotropy,
    std::vector<Vec3> &goals)
{
  NativeTransportV32Result out;
  const auto norm=[](Vec3 v){return std::hypot(v.x,v.y,v.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  if(amount<=0 || radius<=0 || goals.empty())return out;
  const auto n=goals.size();
  std::vector<double> degree(n);
  std::vector<Vec3> change(n);
  const double dn=norm(direction);
  if(dn>1e-12)direction=direction/dn;
  const auto weight=[&](std::size_t a,std::size_t b){
    if(area[a]<=1e-24 || area[b]<=1e-24 ||
       norm(goals[a])<=1e-15 || norm(goals[b])<=1e-15 ||
       dot(goals[a],goals[b])<=0)return 0.0;
    const Vec3 edge=positions[b]-positions[a];const double len=norm(edge);
    if(len<=1e-12 || len>=radius)return 0.0;
    const double align=dn>1e-12?std::abs(dot(edge/len,direction)):1.0;
    return (1.0-anisotropy+anisotropy*align)*std::max(0.0,1.0-len/radius);
  };
  for(const auto &e:edges){const double k=weight(e[0],e[1]);degree[e[0]]+=k;degree[e[1]]+=k;}
  for(const auto &e:edges){
    const auto a=e[0],b=e[1];const double k=weight(a,b);
    if(k<=0)continue;
    const double q=std::clamp(amount,0.0,.35)*k/std::max({1.0,degree[a],degree[b]})*
        std::min(area[a],area[b]);
    const Vec3 transfer=(goals[b]-goals[a])*q;
    change[a]+=transfer/area[a];change[b]+=transfer*(-1.0/area[b]);++out.links;
  }
  if(!out.links)return out;
  double scale=1.0,absolute=0.0;Vec3 budget{};
  for(std::size_t i=0;i<n;++i){
    const double len=norm(change[i]);
    if(len>1e-24)scale=std::min(scale,.30*norm(goals[i])/len);
    absolute+=area[i]*norm(goals[i]);budget+=change[i]*area[i];
  }
  out.relative_budget_error=norm(budget)*scale/std::max(absolute,1e-24);
  if(!std::isfinite(out.relative_budget_error) || out.relative_budget_error>1e-8)return out;
  for(std::size_t i=0;i<n;++i)goals[i]+=change[i]*scale;
  out.correction_scale=scale;
  return out;
}
} // namespace sculpt_physics
