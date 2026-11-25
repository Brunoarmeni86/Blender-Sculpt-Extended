/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physical_brush_intent.hh"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sculpt_physics {
/* Stroke-local geometry, not a persistent per-object material field.
 * Compute a candidate state before native writes, commit only on handled,
 * non-failed dabs. Accepted zero-motion contacts advance the input path. */
struct StrokeContextV27 {
  bool started=false;
  Vec3 center{},axis{0,0,1},direction{};
  double radius=0,pressure=0,path_length=0,normalized_travel=0;
  std::size_t samples=0;
  bool tool_plane_valid=false;
  Vec3 tool_reference{};
  Vec3 transport_direction_v30b{};
  double transport_load_v30b=0.0;
};
inline double stroke_norm_v27(Vec3 p){return std::hypot(p.x,p.y,p.z);}
inline double stroke_dot_v27(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
struct StrokeStepV27 {
  StrokeContextV27 next;
  double dose=0;
  bool gap=false;
};
inline StrokeStepV27 stage_stroke_v27(const StrokeContextV27 &before,Vec3 center,
                                      Vec3 normal,double radius,double pressure)
{
  const double n=stroke_norm_v27(normal);
  if(!std::isfinite(stroke_norm_v27(center)) || !std::isfinite(n) || n<=1e-12 ||
     !std::isfinite(radius) || radius<=1e-12 || !std::isfinite(pressure))
    throw std::invalid_argument("Invalid V27 stroke sample");
  StrokeStepV27 out;
  out.next=before;
  auto &s=out.next;
  const Vec3 axis=normal/n;
  s.center=center;s.radius=radius;s.pressure=std::clamp(pressure,0.0,1.0);
  s.started=true;++s.samples;
  if(!before.started){
    s.axis=axis;s.direction={};out.dose=.15;return out;
  }
  const Vec3 delta=center-before.center;
  const double distance=stroke_norm_v27(delta);
  const double scale=.5*(radius+before.radius);
  if(!std::isfinite(distance) || !std::isfinite(scale) || scale<=1e-12)
    throw std::invalid_argument("Invalid V27 stroke segment");
  s.direction=delta;
  s.path_length+=distance;
  s.normalized_travel+=distance/scale;
  // Do not extrapolate an unobserved segment across a large cursor jump.
  // The swept PBVH implementation must handle these gaps explicitly.
  out.gap=distance>scale;
  out.dose=std::min(1.0,distance/scale);
  const double alignment=stroke_dot_v27(axis,before.axis);
  if(out.gap || alignment<.85){s.axis=axis;}
  else {
    // Spatial, not event-count, smoothing. Keep normal aligned with the
    // current contact; never transport a frame through a flipped surface.
    const double blend=1-std::exp(-distance/(.12*scale));
    const Vec3 mixed=before.axis*(1-blend)+axis*blend;
    s.axis=mixed/stroke_norm_v27(mixed);
  }
  return out;
}
/* V31D5: distribute an optional equivalent click dose over normalized
 * travel, not over input events. Never inject a stationary dose or a
 * bonus across an unobserved gap. The existing first contact is untouched.
 * This is an artistic exposure control, not calibrated physical time. */
inline double continuous_exposure_v31d5(double dose,bool started,bool gap,
                                         double bonus_per_radius)
{
  if(!std::isfinite(dose) || dose<0 || dose>1 ||
     !std::isfinite(bonus_per_radius) || bonus_per_radius<0 || bonus_per_radius>.5)
    throw std::invalid_argument("Invalid V31D5 exposure");
  if(!started || gap || dose==0 || bonus_per_radius==0)return dose;
  return std::min(1.0,dose*(1+bonus_per_radius));
}

/* Stroke-local tool entrainment, not frozen geometry or material inventory. */
inline void stage_transport_v30b(StrokeStepV27 &step,const StrokeContextV27 &before)
{
  auto &s=step.next;
  const auto tangent=[&](Vec3 p){return p-s.axis*stroke_dot_v27(p,s.axis);};
  const Vec3 motion=tangent(s.direction);
  const double distance=stroke_norm_v27(motion);
  Vec3 prior=tangent(before.transport_direction_v30b);
  const double len=stroke_norm_v27(prior);
  if(len>1e-12)prior=prior/len;else prior={};
  if(distance<=1e-12){
    s.transport_direction_v30b=step.gap?Vec3{}:prior;
    s.transport_load_v30b=step.gap?0.0:before.transport_load_v30b;
    return;
  }
  const Vec3 current=motion/distance;
  const bool reset=step.gap || len<=1e-12 || stroke_dot_v27(prior,current)<0;
  const double blend=1-std::exp(-distance/(.22*s.radius));
  const Vec3 mixed=reset?current:prior*(1-blend)+current*blend;
  s.transport_direction_v30b=mixed/stroke_norm_v27(mixed);
  const double load=reset?0.0:before.transport_load_v30b;
  s.transport_load_v30b=std::clamp(load+(1-load)*(1-std::exp(-distance/(.25*s.radius))),0.0,1.0);
}
/* Follow supported leading samples rather than the bottom of a newly
 * carved track. Only existing native candidates participate. */
inline bool track_tool_plane_v27(StrokeStepV27 &step,const StrokeContextV27 &before,
                                const std::vector<PhysicalBrushIntentSample> &samples,
                                bool recover_v28b=false)
{
  auto &s=step.next;
  const Vec3 axis=s.axis;
  const Vec3 tangent=s.direction-axis*stroke_dot_v27(s.direction,axis);
  const double distance=stroke_norm_v27(tangent);
  if(!before.tool_plane_valid || step.gap || stroke_dot_v27(axis,before.axis)<.85){
    s.tool_reference=s.center;
    s.tool_plane_valid=true;
    return true;
  }
  const Vec3 move=s.center-before.tool_reference;
  s.tool_reference=before.tool_reference+move-axis*stroke_dot_v27(move,axis);
  if(distance<=1e-12)return false;
  const Vec3 dir=tangent/distance;
  double total=0,height=0,fallback_total=0,fallback_height=0;
  for(const auto &v:samples){
    if(!v.writable || v.envelope<=0 || !std::isfinite(v.envelope))continue;
    const Vec3 q=v.position-s.center;
    const double z=stroke_dot_v27(q,axis);
    const Vec3 p=q-axis*z;
    const double front=stroke_dot_v27(p,dir)/s.radius;
    const double r=stroke_norm_v27(p)/s.radius;
    if(!std::isfinite(r) || !std::isfinite(z) || r>=.95 || std::abs(z)>.5*s.radius)continue;
    const double n=stroke_norm_v27(v.surface_normal);
    if(n>1e-12 && stroke_dot_v27(v.surface_normal/n,axis)<.5)continue;
    if(recover_v28b && front>0 && r>.15){
      const double weight=v.envelope*std::clamp(front/.20,0.0,1.0)*std::clamp((.95-r)/.20,0.0,1.0);
      fallback_total+=weight;fallback_height+=weight*z;
    }
    if(front<=.20)continue;
    const double weight=v.envelope*std::min(1.0,(front-.20)/.25)*
                        std::clamp((.95-r)/.20,0.0,1.0);
    height+=weight*z;total+=weight;
  }
  if(total<=1e-12 && recover_v28b && fallback_total>1e-12){
    total=fallback_total;height=fallback_height;
  }
  if(total<=1e-12)return false;
  const double old=stroke_dot_v27(s.tool_reference-s.center,axis);
  const double desired=height/total;
  const double blend=1-std::exp(-distance/(.18*s.radius));
  const double change=std::clamp((desired-old)*blend,-.40*distance,.40*distance);
  s.tool_reference+=axis*change;
  return true;
}
/* Analytic swept-sphere membership (constant radius). Not yet connected
 * to Blender PBVH; provides a testable primitive for the next integration.
 * Degenerate segments reduce to a point, never divide by zero. */
inline double swept_distance_squared_v27(Vec3 p,Vec3 a,Vec3 b)
{
  const Vec3 ab=b-a;
  const double den=stroke_dot_v27(ab,ab);
  const double t=den>1e-24?std::clamp(stroke_dot_v27(p-a,ab)/den,0.0,1.0):0.0;
  const Vec3 d=p-(a+ab*t);
  return stroke_dot_v27(d,d);
}
} // namespace sculpt_physics
