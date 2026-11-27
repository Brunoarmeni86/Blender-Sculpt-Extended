/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "d12_clay_engine.hh"
#include "artistic_paired_flux.hh"
#include <array>
#include <string_view>

namespace sculpt_physics::dedicated_d14::bulb {
namespace d=dedicated_d12;
using Edge=std::array<std::size_t,2>;
inline bool asset_name(std::string_view name){
 constexpr std::string_view needle="claybulb";
 std::size_t matched=0;
 for(char c:name){
  if(c>='A'&&c<='Z')c=char(c-'A'+'a');
  if(c<'a'||c>'z')continue;
  if(c==needle[matched]){
   if(++matched==needle.size())return true;
  }else matched=c==needle[0]?1:0;
 }
 return false;
}
struct Kernel {
 Vec3 start{},end{},axis{};
 double radius=1.,dose=0.,intensity=0.,sign=1.;
 double gain=.27,stroke_height=.30;
};
// Compact C2 convex profile. No moving plane, negative gap, or flat core.
// Clip the interval to the projected circular tip. Four-point Gauss-Legendre
// is exact for the degree-six radial polynomial at constant depth.
inline double profile_integral(const Kernel& k,Vec3 p){
 if(!d::finite(p)||!d::finite(k.start)||!d::finite(k.end)||!d::finite(k.axis)||
    !std::isfinite(k.radius)||k.radius<=0.||d::norm2(k.axis)<.5)return 0.;
 const Vec3 q=(p-k.start)/k.radius,a=(k.end-k.start)/k.radius;
 const double h0=d::dot(q,k.axis),dh=d::dot(a,k.axis);
 const Vec3 u=q-k.axis*h0,v=a-k.axis*dh;
 const double vv=d::norm2(v);
 double lo=0.,hi=1.;
 if(vv>1e-20){
  const double middle=d::dot(u,v)/vv;
  const double min_r2=d::norm2(u-v*middle);
  if(min_r2>=1.)return 0.;
  const double half=std::sqrt(std::max(0.,1.-min_r2)/vv);
  lo=std::max(0.,middle-half);hi=std::min(1.,middle+half);
 }else if(d::norm2(u)>=1.)return 0.;
 if(hi<=lo)return 0.;
 constexpr double nodes[4]={-.8611363115940526,-.3399810435848563,.3399810435848563,.8611363115940526};
 constexpr double weights[4]={.3478548451374539,.6521451548625461,.6521451548625461,.3478548451374539};
 double sum=0.;
 for(int i=0;i<4;++i){
  const double t=.5*(lo+hi)+.5*(hi-lo)*nodes[i];
  const double x=std::max(0.,1.-d::norm2(u-v*t));
  const double depth=d::smooth01((.55-std::abs(h0-dh*t))/.25);
  sum+=weights[i]*x*x*x*depth;
 }
 return std::clamp(sum*.5*(hi-lo),0.,1.);
}
inline Vec3 deposit(const Kernel& k,const PhysicalBrushIntentSample& s,
                    const d::ClayReference& ref){
 if(!d::valid_sample(s)||!d::finite(ref.position)||!d::finite(ref.normal)||
    !std::isfinite(ref.radius)||ref.radius<=0.||
    !std::isfinite(k.dose)||k.dose<=0.||
    !std::isfinite(k.intensity)||k.intensity<=0.||
    !std::isfinite(k.gain)||k.gain<=0.||
    !std::isfinite(k.stroke_height)||k.stroke_height<=0.||
    (k.sign!=1.&&k.sign!=-1.))return {};
 const double facing=d::dot(d::unit_normal(s.surface_normal),k.axis);
 const auto original=d::unit_normal(ref.normal);
 if(facing<=.05||d::dot(original,k.axis)<=.05)return {};
 const double support=profile_integral(k,s.position);
 if(support<=0.)return {};
 const double cap=k.stroke_height*std::min(k.radius,ref.radius);
 const double remaining=std::max(0.,cap-d::norm(s.position-ref.position));
 if(remaining<=0.)return {};
 const double exposure=k.gain*k.radius*k.dose*k.intensity*s.envelope*support*
     d::smooth01((facing-.05)/.55);
 // Additive deposition saturates smoothly relative to the stroke reference;
 // the live surface is never a moving target plane.
 const double amount=remaining*(-std::expm1(-exposure/cap));
 const Vec3 direction=d::unit(k.axis*.70+original*.30);
 return direction*(k.sign*amount);
}
// Fuse the new deposit against the LIVE relief, including earlier strokes.
// Pairwise residual measured against the average local normal suppresses
// transport along a simple inclined plane or locally smooth curved surface.
// No old material removal; no implicit generic Smooth. The conserved
// quantity is lumped-area axial addition before Blender's final guard.
inline bool fuse(const Kernel& k,const std::vector<PhysicalBrushIntentSample>& s,
 const std::vector<Edge>& edges,const std::vector<double>& area,
 const std::vector<double>& headroom,std::vector<Vec3>& motion,
 PairedFluxBackend* gpu=nullptr,bool* used=nullptr,double* transferred=nullptr){
 if(used)*used=false;
 if(transferred)*transferred=0.;
 const auto n=s.size();
 if(area.size()!=n||headroom.size()!=n||motion.size()!=n||
    !d::finite(k.axis)||d::norm2(k.axis)<.5||!std::isfinite(k.radius)||k.radius<=0.)return false;
 std::vector<double> give(n),take(n),degree(n);
 for(std::size_t i=0;i<n;++i){
  if(!std::isfinite(area[i])||area[i]<0.||!std::isfinite(headroom[i])||headroom[i]<0.||!d::finite(motion[i]))return false;
  if(!d::valid_sample(s[i])||area[i]<=1e-24)continue;
  const double h=std::max(0.,d::dot(motion[i],k.axis));
  if(h<=1e-15)continue;
  give[i]=.55*h*area[i];
  take[i]=std::min({headroom[i],1.25*h,.04*k.radius})*area[i];
 }
 const auto eligible=[&](Edge e){
  const auto a=e[0],b=e[1];
  return a!=b&&give[a]>0.&&give[b]>0.&&
      d::norm2(s[a].position-s[b].position)<.25*k.radius*k.radius&&
      d::dot(d::unit_normal(s[a].surface_normal),d::unit_normal(s[b].surface_normal))>.35;
 };
 for(auto e:edges){
  if(e[0]>=n||e[1]>=n)return false;
  if(eligible(e)){degree[e[0]]++;degree[e[1]]++;}
 }
 std::vector<PairedFluxLink> links;
 for(auto e:edges)if(eligible(e)){
  auto a=e[0],b=e[1];
  const auto normal=d::unit(d::unit_normal(s[a].surface_normal)+d::unit_normal(s[b].surface_normal));
  if(d::dot(normal,k.axis)<=.2)continue;
  double residual=d::dot((s[a].position+motion[a])-(s[b].position+motion[b]),normal);
  if(residual<0.){std::swap(a,b);residual=-residual;}
  if(take[b]<=0.)continue;
  const double q=.45*residual*std::min(area[a],area[b])/std::max({1.,degree[a],degree[b]});
  if(q>0.)links.push_back({a,b,q});
 }
 if(links.empty())return true;
 std::vector<double> delta;double moved=0.;
 const bool accelerated=paired_flux_dispatch(gpu,links,give,take,delta,moved);
 if(used)*used=accelerated;
 if(transferred)*transferred=moved;
 for(std::size_t i=0;i<n;++i)if(delta[i]!=0.)motion[i]+=k.axis*(delta[i]/area[i]);
 return true;
}
} // namespace sculpt_physics::dedicated_d14::bulb
