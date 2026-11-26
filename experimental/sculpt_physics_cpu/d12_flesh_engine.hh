/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independent analytical field using regularized Kelvinlets (de Goes &
 * James 2017), inspired by SculptCore kelvinlet.sbrush. No imported source.
 * Surface gesture approximation, not volumetric flesh or actual cloth.
 */
#pragma once
#include "d12_brush_common.hh"

namespace sculpt_physics::dedicated_d12 {
struct FleshParams {
 double poisson=.47;
 double adhesion=.8;
 double viscosity=.25; // spatial lag in radius units
 double bulge=.20; // artistic compression response
 double max_depth=.5;
};
struct FleshState {bool valid=false;Vec3 velocity{};};
struct FleshKernel {
 Vec3 center{},axis{},drag{},direction{};
 double radius=1.,ratio=0.,bulge=0.,max_depth=.5,drag_length=0.;
 FleshState next{};
};
// Reduce once per dab. Drag measured in object units, filtered spatially.
// No persistent mesh field or solver steps in this first reference.
inline FleshKernel prepare_flesh(Vec3 center,Vec3 axis,Vec3 motion,double radius,
 double strength,const FleshState& previous,const FleshParams& p){
 if(!finite(center)||!finite(axis)||!finite(motion)||norm(axis)<1e-12||
    !std::isfinite(radius)||radius<=0||!std::isfinite(strength)||strength<0||
    !std::isfinite(p.poisson)||p.poisson<0||p.poisson>=.5||
    !std::isfinite(p.adhesion)||p.adhesion<0||p.adhesion>1||
    !std::isfinite(p.viscosity)||p.viscosity<0||
    !std::isfinite(p.bulge)||p.bulge<0||p.bulge>1||
    !std::isfinite(p.max_depth)||p.max_depth<=0)
  throw std::invalid_argument("D12 flesh parameters");
 axis=unit(axis);
 const double travel=norm(motion)/radius;
 Vec3 tangent=motion-axis*dot(motion,axis);
 Vec3 direction=unit(tangent);
 // No stationary creep or long jumps after loss of contact.
 if(travel<=1e-9||travel>1.5||norm(direction)<.5)
  return {center,axis,{}, {},radius,0.,p.bulge,p.max_depth,0.,{}};
 if(previous.valid&&p.viscosity>0){
  const double a=std::exp(-travel/p.viscosity);
  const Vec3 old=previous.velocity-axis*dot(previous.velocity,axis);
  const Vec3 blend=direction*(1.-a)+old*a;
  if(norm(blend)>1e-6&&dot(blend,direction)>.0)direction=unit(blend);
 }
 const double length=std::min(norm(tangent)*strength*p.adhesion,.12*radius);
 // b/a = 1/(4(1-nu)); normalization makes center follow drag exactly.
 const double ratio=1./(4.*(1.-p.poisson));
 return {center,axis,direction*length,direction,radius,ratio,p.bulge,
         p.max_depth,length,{true,direction}};
}
inline Vec3 flesh_vertex(const FleshKernel& k,const PhysicalBrushIntentSample& s){
 if(!valid_sample(s)||k.drag_length==0)return {};
 const Vec3 local_n=unit_normal(s.surface_normal);
 if(dot(local_n,k.axis)<.05)return {};
 const Vec3 q=(s.position-k.center)/k.radius;
 if(std::abs(dot(q,k.axis))>k.max_depth)return {};
 const double r2=dot(q,q);
 if(r2>=1.)return {};
 const double t=1./std::sqrt(1.+r2);
 const Vec3 qn=q*t;
 const double den=1.5-k.ratio;
 // Regularized point force, normalized at the origin. No arbitrary
 // elasticity multiplier in the inner loop. Direction stays tangent-ish.
 const double iso=t*(1.-.5*(1.-t*t)/den);
 const double anis=k.ratio*t/den;
 Vec3 d=k.drag*iso+qn*(dot(k.drag,qn)*anis);
 // The compression-side lift and trailing-side depression are a bounded
 // artistic surface proxy; not an assertion of volume conservation.
 const double lead=std::clamp(dot(q,k.direction),-1.,1.);
 d+=local_n*(k.drag_length*k.bulge*lead*(1.-r2));
 return d*s.envelope;
}
}
