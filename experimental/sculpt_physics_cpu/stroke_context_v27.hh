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
