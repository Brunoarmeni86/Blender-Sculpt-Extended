/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "artistic_geometry_v31d1110.hh"
#include "clay_surface_solver.hh"

namespace sculpt_physics::artistic_v31d1110 {
// Reuse V30's paired brush-scale relief transport. Local chart only;
// no volumetric simulation. Separate disconnected writable regions and
// reject folded charts rather than project opposite sheets onto one grid.
inline std::vector<Vec3> relief_target(const h::Patch& patch, Action action,
 Material material,Vec3 center,Vec3 direction,Vec3 tool_axis,
 double radius,double dose,double pressure,bool fast=false,
 const std::vector<double>* compaction=nullptr,
 PairedFluxBackend* accelerator=nullptr,bool* gpu_used=nullptr){
 if(action!=Action::Push&&action!=Action::Smear&&action!=Action::Gather&&action!=Action::Melt)
  throw std::invalid_argument("relief action");
 if(!std::isfinite(radius)||radius<=0||!std::isfinite(dose)||dose<0||
    !std::isfinite(h::norm(center))||!std::isfinite(h::norm(direction))||
    !std::isfinite(h::norm(tool_axis)))throw std::invalid_argument("relief inputs");
 if(gpu_used)*gpu_used=false;
 const auto response=evaluate(material,pressure,0,1);
 const auto profile_data=profile(material);
 const auto n=patch.p.size();
 if(compaction&&compaction->size()!=n)throw std::invalid_argument("relief compaction shape");
 std::vector<Vec3> out(n);
 if(dose==0||(action==Action::Smear?response.drag:response.transport)==0)return out;
 std::vector<unsigned char> seen(n);
 std::size_t components=0;
 for(std::size_t seed=0;seed<n;++seed){
  if(seen[seed]||patch.weight[seed]<=0||patch.area[seed]<=0)continue;
  std::vector<std::size_t> ids{seed};seen[seed]=1;
  for(std::size_t k=0;k<ids.size();++k)for(auto j:patch.adj[ids[k]])
   if(!seen[j]&&patch.weight[j]>0&&patch.area[j]>0){
    seen[j]=1;ids.push_back(j);
   }
  if(ids.size()<3)continue;
  if(++components>8)throw std::invalid_argument("relief component budget");
  Vec3 axis{};
  for(auto i:ids)axis+=patch.normal[i]*(patch.area[i]*patch.weight[i]);
  axis=h::unit(axis);
  if(h::norm(axis)<.5)throw std::invalid_argument("ambiguous relief chart");
  for(auto i:ids)if(h::dot(axis,patch.normal[i])<.35)
   throw std::invalid_argument("folded relief chart");
  if(h::dot(axis,tool_axis)<-.05)throw std::invalid_argument("back relief chart");
  std::vector<Vec3> xy(n);
  std::vector<double> height(n),weight(n),delta(n);
  std::vector<std::array<std::size_t,2>> edges;
  double floor=std::numeric_limits<double>::infinity();
  for(auto i:ids){
   const auto q=patch.p[i]-center;
   height[i]=h::dot(q,axis);xy[i]=q-axis*height[i];
   floor=std::min(floor,height[i]);
   // Keep the host permission unchanged; hardened clay participates
   // less in the conservative proxy transport, never across pinned regions.
   const double c=compaction?(*compaction)[i]:0.;
   if(!std::isfinite(c)||c<0||c>1)throw std::invalid_argument("relief compaction");
   weight[i]=patch.weight[i]/(1.+profile_data.hardening*c);
   for(auto j:patch.adj[i])if(i<j&&patch.weight[j]>0&&patch.area[j]>0)
    edges.push_back({i,j});
  }
  ClaySurfaceDab dab;
  dab.action=action==Action::Push?ClaySurfaceAction::Push:
             action==Action::Smear?ClaySurfaceAction::Smear:
             action==Action::Gather?ClaySurfaceAction::Gather:ClaySurfaceAction::Melt;
  dab.center=center;dab.axis=axis;dab.direction=direction;dab.radius=radius;
  dab.stable_v28=true;dab.smooth_proxy=true;dab.density_independent=true;
  dab.live_surface_v30=true;dab.transport_v30b=action!=Action::Melt;
  dab.refine_v28b=true;dab.melt_fallback_v31c=true;
  dab.push_v30d=action==Action::Push;dab.push_gain_v30d=2.5;
  dab.detail_retention=std::clamp(1.-profile_data.flow,0.,1.);
  dab.tool_v26=false; // no implicit gravity
  dab.artistic_flux_backend=accelerator;
  ClaySurfaceResult diagnostics;
  // Flow is a material response; adhesion separately modulates smear.
  // Smear is adhesion-led, Push is flow-led. Gather uses the radial
  // field; Melt has diffusion only. Avoid multiplying Smear's yield twice.
  const double mobility=action==Action::Smear?response.drag:response.transport;
  const double drag=action==Action::Smear?2.0:1.;
  const double travel=dose*(action==Action::Gather?1.4:action==Action::Melt?1.8:1.);
  coarse_clay_transport(xy,height,weight,patch.area,dab,axis,floor-.12*radius,
      travel,mobility,profile_data.yield,drag,1.,delta,diagnostics,&edges);
  if(gpu_used)*gpu_used=*gpu_used||diagnostics.artistic_flux_gpu;
  for(auto i:ids)out[i]=patch.normal[i]*delta[i];
 }
 // Area-weighted normal-height proxy; the existing optimizer retains
 // the feasible signed proxy, not exact enclosed volume.
 return fast?accept_fast(patch,std::move(out)):h::optimize(patch,std::move(out),true);
}

// Preserve the native deposition vector; redistribute only its positive
// normal relief, with stronger lateral spreading for softer profiles.
inline std::vector<Vec3> spread_deposit(const h::Patch& patch,
 std::vector<Vec3> motion,Material material,double radius,double dose,
 double pressure,h::GeodesicCache* cache=nullptr,bool fast=false){
 if(motion.size()!=patch.p.size()||!std::isfinite(radius)||radius<=0||
    !std::isfinite(dose)||dose<0)throw std::invalid_argument("deposit spread");
 auto response=evaluate(material,pressure,0,1);
 if(dose==0||response.transport==0)return fast?accept_fast(patch,std::move(motion)):h::optimize(patch,std::move(motion),true);
 std::vector<double> drive(motion.size());
 for(std::size_t i=0;i<drive.size();++i)if(patch.weight[i]>0)
  drive[i]=std::clamp(h::dot(patch.normal[i],motion[i])/dose,0.,1.)*response.transport;
 auto flux=pair_transport(patch,drive,radius,dose,cache);
 // Bound exchange by actual positive deposition at each donor.
 double scale=1.;
 for(std::size_t i=0;i<flux.size();++i)if(flux[i]<0)
  scale=std::min(scale,std::max(0.,h::dot(motion[i],patch.normal[i]))/-flux[i]);
 for(std::size_t i=0;i<flux.size();++i)motion[i]+=patch.normal[i]*(scale*flux[i]);
 return fast?accept_fast(patch,std::move(motion)):h::optimize(patch,std::move(motion),true);
}
}
