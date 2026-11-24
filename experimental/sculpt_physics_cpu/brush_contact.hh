/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physical_brush_intent.hh"
#include <array>
#include <unordered_map>

namespace sculpt_physics {

/* V19-C contact state belongs to one native stroke, never to the mesh.
 * The reference is captured before the first eligible contact at a vertex.
 * Repeated compression is limited relative to that reference, not by a
 * per-dab counter. A new stroke can intentionally compress further.
 * No persistent constitutive material history or volumetric solver. */
struct BrushContactReference {
  Vec3 position{};
  Vec3 axis{};
};
using BrushContactHistory=std::unordered_map<std::size_t,BrushContactReference>;

inline std::vector<NativeBrushTranslation> plan_drag_contact(
    const std::vector<PhysicalBrushIntentSample> &samples,
    const PhysicalBrushIntentDab &dab,
    const BrushContactHistory &history)
{
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto norm=[](Vec3 v){return std::hypot(v.x,v.y,v.z);};
  const auto unit=[&](Vec3 v){const double l=norm(v);return l>1e-12?v/l:Vec3{};};
  // Reuse the existing input validation and Deposit baseline.
  auto baseline=plan_physical_brush_intent(samples,dab);
  if(dab.action==PhysicalBrushAction::Deposit) return baseline;
  if(dab.dose==0 || dab.intensity==0) return {};
  const Vec3 axis=unit(dab.normal);
  const Vec3 direction=unit(dab.stroke_direction-axis*dot(dab.stroke_direction,axis));
  const bool moving=norm(direction)>1e-12;
  const double step=0.1*dab.radius*dab.intensity*dab.dose;
  std::vector<NativeBrushTranslation> goals;
  goals.reserve(samples.size());
  for(const auto &s:samples) {
    if(!s.writable || s.envelope<=0) continue;
    const Vec3 offset=(s.position-dab.center)/dab.radius;
    const Vec3 radial=offset-axis*dot(offset,axis);
    const double r=norm(radial);
    if(std::abs(dot(offset,axis))>0.65 ||
       (dab.limit_to_projected_disk && r>1)) continue;
    Vec3 n=unit(s.surface_normal);
    if(norm(n)<=1e-12) n=axis*dab.radial_sign;
    // Ctrl changes the force axis, not which side of the surface is exposed.
    if(dot(n,axis)*dab.radial_sign<0.25) continue;
    const auto tangent=[&](Vec3 x){return x-n*dot(x,n);};
    const Vec3 drag=tangent(direction);
    const Vec3 side=tangent(radial/std::max(1.0,r))*dab.radial_sign;
    const double h=step*s.envelope;
    Vec3 move{};
    switch(dab.action) {
      case PhysicalBrushAction::Deposit: break;
      case PhysicalBrushAction::Compress:
      case PhysicalBrushAction::Compact:
      case PhysicalBrushAction::Press: {
        const auto it=history.find(s.global_index);
        const auto ref=it==history.end()?BrushContactReference{s.position,axis}:it->second;
        const double already=std::max(0.0,dot(ref.position-s.position,ref.axis));
        const double limit=0.12*dab.radius*dab.intensity;
        // Use the actual geometry after previous accepted writes. Do not
        // advance a hidden compaction counter on rejected/zero dabs.
        double depth=std::min(h,std::max(0.0,limit-already));
        // A changed tool normal must not circumvent the original contact
        // reference. Reject strongly incompatible orientation this stroke.
        if(dot(axis,ref.axis)<0.5) depth=0;
        if(dab.action==PhysicalBrushAction::Press)
          depth=std::min(depth,std::max(0.0,dot(s.position-dab.center,axis)+step)*s.envelope);
        const double lateral=dab.action==PhysicalBrushAction::Compact?0.12:0.25;
        move=axis*(-depth)+ (moving ? drag*0.8+side*0.2 : side*0.35)*
                                (depth*lateral*dab.spread);
        break;
      }
      case PhysicalBrushAction::Flow:
        move=drag*h;
        break;
      case PhysicalBrushAction::PressureFlow:
        // Sliding contact rather than pure radial stretching. Stationary
        // action is deliberately much weaker than a moving tool.
        move=(moving?drag*0.75+side*0.25:side*0.20)*h;
        break;
      case PhysicalBrushAction::HybridFlow:
        move=(moving?drag*(1.0-0.25*dab.spread)+side*(0.25*dab.spread):side*0.20)*h;
        break;
    }
    if(norm(move)>1e-15) goals.push_back({s.global_index,move});
  }
  std::sort(goals.begin(),goals.end(),[](const auto &a,const auto &b){
    return a.global_index<b.global_index;
  });
  return goals;
}

/* Preserve signed projected area throughout the linear dab trajectory.
 * Unlike the absolute-coefficient bound, a triangle opening safely does
 * not suppress motion. Check the endpoint and any interior minimum of
 * the signed quadratic. Common scale preserves linear flux budgets.
 * No global collision detection or repair of existing folds. */
struct ContactTriangle {
  std::array<Vec3,3> position;
  std::array<Vec3,3> motion;
};
inline double contact_triangle_scale(const ContactTriangle &tri,double retained=0.35)
{
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{
    a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto norm=[](Vec3 v){return std::hypot(v.x,v.y,v.z);};
  if(!std::isfinite(retained)||retained<=0||retained>=1)
    throw std::invalid_argument("Invalid contact area limit");
  for(const auto &v:tri.position) if(!std::isfinite(norm(v)))
    throw std::invalid_argument("Invalid contact triangle position");
  for(const auto &v:tri.motion) if(!std::isfinite(norm(v)))
    throw std::invalid_argument("Invalid contact triangle motion");
  const Vec3 a=tri.position[1]-tri.position[0],b=tri.position[2]-tri.position[0];
  const Vec3 da=tri.motion[1]-tri.motion[0],db=tri.motion[2]-tri.motion[0];
  if(norm(da)==0 && norm(db)==0) return 1;
  const Vec3 n=cross(a,b);
  const double area=norm(n);
  if(!std::isfinite(area)) throw std::overflow_error("Contact triangle area overflow");
  if(area<=1e-24) return 0;
  const Vec3 u=n/area;
  // Normalize by initial area: the constraint is dimensionless.
  const double l=dot(cross(da,b)+cross(a,db),u)/area;
  const double q=dot(cross(da,db),u)/area;
  if(!std::isfinite(l)||!std::isfinite(q)) throw std::overflow_error("Contact motion overflow");
  const double budget=1-retained;
  const auto safe=[&](double t) {
    const double endpoint=t*(l+t*q);
    if(!std::isfinite(endpoint)) return false;
    double minimum=std::min(0.0,endpoint);
    if(q>0 && l<0) {
      const double critical=(-0.5*l)/q;
      if(critical<t) minimum=std::min(minimum,critical*(l+critical*q));
    }
    return minimum>=-budget;
  };
  if(safe(1)) return 1;
  double lo=0,hi=1;
  for(int i=0;i<40;++i) {
    const double mid=(lo+hi)*0.5;
    if(safe(mid)) lo=mid; else hi=mid;
  }
  return lo;
}

} // namespace sculpt_physics
