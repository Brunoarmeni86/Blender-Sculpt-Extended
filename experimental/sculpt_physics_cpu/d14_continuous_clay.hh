/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "d14_clay_bulb.hh"
namespace sculpt_physics::dedicated_d14::continuous_clay {
namespace d=dedicated_d12;
namespace b=bulb;
inline double fade(double x){
 x=std::clamp(x,0.,1.);return x*x*x*(10.+x*(-15.+6.*x));
}
struct Kernel {
 Vec3 start{},end{},axis{},tangent{};
 double radius=1.,dose=0.,intensity=0.,roundness=1.,gain=.22;
};
// Additive deposition: no target plane, no uniform cumulative stroke height.
// A compact profile has zero value and first/second derivatives at its rim.
// Round tool uses the existing clipped quadrature; square/mixed tool has
// its own cross-section and bounded midpoint integration.
inline double profile(const Kernel& k,Vec3 p){
 const double r=.94*k.radius; // small permitted collar inside native footprint
 if(k.roundness>=.999){
  b::Kernel q{k.start,k.end,k.axis,r,k.dose,k.intensity,1.};
  return b::profile_integral(q,p);
 }
 const auto tangent=d::unit(k.tangent-k.axis*d::dot(k.tangent,k.axis));
 const auto side=d::unit(d::cross(k.axis,tangent));
 if(d::norm2(tangent)<.5||d::norm2(side)<.5)return 0.;
 const auto delta=k.end-k.start;
 const int count=std::clamp(int(std::ceil(d::norm(delta)/r/.06)),1,32);
 double sum=0.;
 for(int i=0;i<count;++i){
  const auto q=(p-k.start-delta*((i+.5)/count))/r;
  const double x=d::dot(q,tangent),y=d::dot(q,side);
  const double shape=(1.-k.roundness)*std::max(std::abs(x),std::abs(y))+
                     k.roundness*std::sqrt(x*x+y*y);
  const double v=std::max(0.,1.-shape*shape);
  sum+=v*v*v*fade((.55-std::abs(d::dot(q,k.axis)))/.25);
 }
 return sum/count;
}
inline Vec3 deposit(const Kernel& k,const PhysicalBrushIntentSample& s){
 if(!d::valid_sample(s)||!d::finite(k.start)||!d::finite(k.end)||
    !d::finite(k.axis)||std::abs(d::norm2(k.axis)-1.)>1e-5||
    !std::isfinite(k.radius)||k.radius<=0.||!std::isfinite(k.dose)||k.dose<=0.||k.dose>1.5||
    !std::isfinite(k.intensity)||k.intensity<=0.||
    !std::isfinite(k.roundness)||k.roundness<0.||k.roundness>1.||
    !std::isfinite(k.gain)||k.gain<=0.)return {};
 const auto n=d::unit_normal(s.surface_normal);
 const double facing=fade((d::dot(n,k.axis)-.05)/.40);
 if(facing<=0.)return {};
 const double support=profile(k,s.position);
 const double raw=k.gain*k.radius*k.dose*k.intensity*s.envelope*support*facing;
 // Only anomalously large single dabs are limited. Ordinary dabs add
 // linearly with arc-length dose; there is no stroke-wide saturation.
 const double amount=std::min(raw,.16*k.radius);
 return k.axis*amount;
}
}
