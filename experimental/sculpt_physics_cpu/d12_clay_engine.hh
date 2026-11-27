/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independent implementation inspired by the plane-family idea in
 * SculptCore (Joseph Eagar, MIT): source/brush/kernels/plane.sbrush,
 * source/brush/plane_frame.h. No imported SculptCore source.
 */
#pragma once
#include "d12_brush_common.hh"

namespace sculpt_physics::dedicated_d12 {
struct Frame {bool valid=false;Vec3 center{},axis{},tangent{};};
struct ClayParams {
 double offset=.105; // plane elevation in radius units
 double rate=4.; // response per radius traveled
 double roundness=.55; // 0 squared, 1 circular
 double sample_radius=.75;
 double stability=.28; // spatial smoothing length in radii
 double max_depth=.5; // reject opposite/deep sheet
 double stroke_height=.24; // maximum displacement from first touch in this stroke
};
// Use live native samples and a bounded front-facing area frame. No adjacency
// gather or allocation. State is staged and committed only by the caller.
inline Frame clay_frame(const std::vector<PhysicalBrushIntentSample>& samples,
 Vec3 center,Vec3 hit_normal,Vec3 delta,double radius,const Frame& previous,
 const ClayParams& p){
 if(!finite(center)||!finite(hit_normal)||!finite(delta)||
    !std::isfinite(radius)||radius<=0||norm(hit_normal)<1e-12||
    !std::isfinite(p.sample_radius)||p.sample_radius<=0||
    !std::isfinite(p.stability)||p.stability<0)throw std::invalid_argument("D12 frame");
 const Vec3 n=unit(hit_normal);
 Vec3 sum{};double height=0.,weight=0.;
 const double reach=radius*p.sample_radius;
 for(const auto& v:samples)if(valid_sample(v)){
  const Vec3 q=v.position-center;
  const double r=norm(q)/reach;
  if(r>=1.||dot(unit_normal(v.surface_normal),n)<.2)continue;
  const double w=(1.-r*r)*(1.-r*r);
  sum+=unit_normal(v.surface_normal)*w;
  height+=dot(q,n)*w;weight+=w;
 }
 Vec3 axis=norm(sum)>1e-12?unit(sum):n;
 Vec3 origin=center+n*(weight>0?std::clamp(height/weight,-.12*radius,.12*radius):0.);
 const double travel=norm(delta)/radius;
 const bool continuous=previous.valid&&travel<=1.5&&dot(previous.axis,axis)>.35;
 if(continuous&&p.stability>0){
  const double a=std::exp(-std::max(travel,.025)/p.stability);
  axis=unit(axis*(1.-a)+previous.axis*a);
  // Stabilize only signed plane height, never lag the cursor tangentially.
  const double side=dot(origin-previous.center,previous.axis);
  origin=origin-previous.axis*std::clamp(side*a,-.10*radius,.10*radius);
 }
 Vec3 tangent=unit(delta-axis*dot(delta,axis));
 if(norm(tangent)<.5&&continuous)tangent=unit(previous.tangent-axis*dot(previous.tangent,axis));
 if(norm(tangent)<.5)tangent=unit(cross(std::abs(axis.z)<.9?Vec3{0,0,1}:Vec3{0,1,0},axis));
 return {true,origin,axis,tangent};
}
struct ClayKernel {
 Frame frame;
 Vec3 transverse{};
 double radius=1.,offset=0.,exposure=0.,roundness=1.,max_depth=.5,sign=1.,stroke_height=.24;
};
inline ClayKernel prepare_clay(Frame frame,double radius,double dose,double strength,double sign,
 const ClayParams& p){
 if(!frame.valid||!finite(frame.center)||!finite(frame.axis)||!finite(frame.tangent)||
    !std::isfinite(radius)||radius<=0||!std::isfinite(dose)||dose<0||
    !std::isfinite(strength)||strength<0||(sign!=1.&&sign!=-1.)||
    !std::isfinite(p.offset)||p.offset<0||p.offset>.5||
    !std::isfinite(p.rate)||p.rate<0||
    !std::isfinite(p.roundness)||p.roundness<0||p.roundness>1||
    !std::isfinite(p.max_depth)||p.max_depth<=0||
    !std::isfinite(p.stroke_height)||p.stroke_height<=0||p.stroke_height>1.)
  throw std::invalid_argument("D12 clay parameters");
 return {frame,unit(cross(frame.axis,frame.tangent)),radius,p.offset,
         std::min(16.,p.rate*dose*strength),p.roundness,p.max_depth,sign,p.stroke_height};
}
struct ClayReference {Vec3 position{},normal{};double radius=1.;};
inline double smooth01(double x){x=std::clamp(x,0.,1.);return x*x*(3.-2.*x);}
// Limit the length from the first accepted stroke contact, not just the
// current dab. This removes the positive feedback of a cursor riding its
// own newly raised ridge. A separate stroke obtains a new reference.
inline Vec3 clay_vertex(const ClayKernel& k,const PhysicalBrushIntentSample& s,
 const ClayReference* reference=nullptr){
 if(!valid_sample(s)||k.exposure==0)return {};
 // Strict support: no back-facing or deep target, and no footprint expansion.
 const Vec3 local_normal=unit_normal(s.surface_normal);
 const double facing=dot(local_normal,k.frame.axis);
 if(facing<=.05)return {};
 const double facing_fade=smooth01((facing-.05)/.55);
 const Vec3 q=(s.position-k.frame.center)/k.radius;
 const double h=dot(q,k.frame.axis)*k.sign;
 if(h < -k.max_depth || h > k.offset)return {};
 const double x=dot(q,k.frame.tangent),y=dot(q,k.transverse);
 const double shape=(1.-k.roundness)*std::max(std::abs(x),std::abs(y))+
                     k.roundness*std::sqrt(x*x+y*y);
 if(shape>=1.)return {};
 // Raised sculpting plane with a rolled shoulder. The native envelope is
 // applied exactly once; the additional shape is tool geometry, not a mask.
 const double tip=1.-smooth01((shape-.52)/.48);
 const double depth_fade=smooth01((h+k.max_depth)/(.35*k.max_depth));
 const double target=k.offset*tip;
 const double gap=std::clamp(target-h,0.,.15);
 const double response=-std::expm1(-k.exposure*s.envelope*tip*depth_fade*facing_fade);
 Vec3 direction=k.frame.axis;
 double amount=k.radius*gap*response;
 if(reference){
  if(!finite(reference->position)||!finite(reference->normal)||
     !std::isfinite(reference->radius)||reference->radius<=0)return {};
  const Vec3 n=unit_normal(reference->normal);
  if(dot(n,k.frame.axis)<.15)return {};
  // Bias toward the original surface normal rather than extruding a
  // tall sidewall along an increasingly tilted, live brush frame.
  direction=unit(n*.65+k.frame.axis*.35);
  const Vec3 accumulated=s.position-reference->position;
  const double cap=k.stroke_height*std::min(k.radius,reference->radius);
  const double used=norm(accumulated);
  if(used>=cap)return {};
  // Smooth saturation before the hard geometric bound. No overshoot.
  amount*=smooth01((cap-used)/(.35*cap));
  amount=std::min(amount,cap-used);
 }
 return direction*(k.sign*amount);
}
}
