/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "d12_smooth_multiscale.hh"
#include <cstring>
#include <cstdlib>
#include <utility>
namespace sculpt_physics::dedicated_d13::flatten {
namespace ms = dedicated_d12::smooth_multiscale;
enum class Mode { Adaptive, Hard, Scrape, Fill, Invalid };
inline Mode parse(const char *s)
{
  if (!s || !*s || std::strcmp(s,"adaptive")==0) return Mode::Adaptive;
  if (std::strcmp(s,"hard")==0) return Mode::Hard;
  if (std::strcmp(s,"scrape")==0) return Mode::Scrape;
  if (std::strcmp(s,"fill")==0) return Mode::Fill;
  return Mode::Invalid;
}
inline const char *name(Mode m)
{
  switch(m){
    case Mode::Adaptive:return "adaptive";
    case Mode::Hard:return "hard";
    case Mode::Scrape:return "scrape";
    case Mode::Fill:return "fill";
    default:return "invalid";
  }
}
struct Params {
  Mode mode=Mode::Adaptive;
  double plane_mix=.65;
  double offset=0.;
};
inline bool number(const char *s,double &value,double low,double high)
{
  if(!s)return true;
  char *end=nullptr;
  const double v=std::strtod(s,&end);
  if(end==s||*end||!std::isfinite(v)||v<low||v>high)return false;
  value=v;
  return true;
}
// D12 chart heights are residuals relative to its fitted affine plane.
// Offsets and limits are dimensionless fractions of the brush radius.
// The input multiscale result is already weighted; apply the weight once.
inline bool evaluate(const ms::Chart &chart,const std::vector<double> &filtered,
                     const Params &p,std::vector<double> &out)
{
  if(p.mode==Mode::Invalid || !std::isfinite(p.plane_mix) ||
     p.plane_mix<0. || p.plane_mix>1. || !std::isfinite(p.offset) ||
     std::abs(p.offset)>.25 || !ms::validate(chart,filtered))return false;
  std::vector<double> staged;
  staged.reserve(chart.points.size());
  for(std::size_t i=0;i<chart.points.size();++i){
    const auto &q=chart.points[i];
    if(!std::isfinite(q.h)||!std::isfinite(q.weight)||q.weight<0.||q.weight>1.)return false;
    const double residual=p.offset-q.h;
    const double direct=std::clamp(.85*residual,-.055,.055)*q.weight;
    double d=direct;
    if(p.mode==Mode::Adaptive)
      d=(1.-p.plane_mix)*filtered[i]+p.plane_mix*direct;
    if(p.mode==Mode::Scrape)d=std::min(d,0.);
    if(p.mode==Mode::Fill)d=std::max(d,0.);
    staged.push_back(d);
  }
  if(!ms::validate(chart,staged))return false;
  out=std::move(staged);
  return true;
}
// V2: separate stroke state, committed only after native writeback.
using sculpt_physics::dedicated_d12::dot;
using sculpt_physics::dedicated_d12::unit;
using sculpt_physics::dedicated_d12::norm2;
using sculpt_physics::dedicated_d12::finite;
struct PlaneState {
  Vec3 point{},normal{},last_center{};
  Mode mode=Mode::Invalid;
  bool valid=false;
};
inline bool update_plane(const ms::Chart &c,Mode mode,double follow,PlaneState &state)
{
  if(!finite(c.plane_point)||!finite(c.plane_normal)||!finite(c.center)||
     norm2(c.plane_normal)<.5||!std::isfinite(c.radius)||c.radius<=0.||
     !std::isfinite(follow)||follow<0.||follow>1.||mode==Mode::Invalid)return false;
  const auto n=unit(c.plane_normal);
  if(!state.valid||state.mode!=mode){
    state={c.plane_point,n,c.center,mode,true};return true;
  }
  if(dot(state.normal,n)<.5)return false;
  // Follow by traveled distance, not dab count. A stationary stroke never
  // drifts its reference plane into the geometry it is modifying.
  const double distance=std::sqrt(norm2(c.center-state.last_center))/c.radius;
  const double blend=-std::expm1(-follow*distance);
  if(blend>0.){
    const auto old_at_center=c.center-state.normal*dot(c.center-state.point,state.normal);
    const auto new_at_center=c.center-n*dot(c.center-c.plane_point,n);
    const double height=dot(new_at_center-old_at_center,state.normal);
    const double limited=std::clamp(height,-.15*c.radius,.15*c.radius);
    state.point=old_at_center+state.normal*(blend*limited);
    state.normal=unit(state.normal*(1.-blend)+n*blend);
    state.last_center=c.center;
  }
  return finite(state.point)&&finite(state.normal)&&norm2(state.normal)>.5;
}
inline double edge_envelope(double r,double start)
{
  const double t=std::clamp((r-start)/(1.-start),0.,1.);
  return 1.-t*t*t*(10.+t*(-15.+6.*t));
}
inline bool evaluate_v2(const ms::Chart &chart,const std::vector<ms::Sample> &samples,
                       const std::vector<double> &filtered,const Params &p,
                       const PlaneState &state,double edge_start,std::vector<double> &out)
{
  if(!state.valid||state.mode!=p.mode||!finite(state.point)||!finite(state.normal)||
     !std::isfinite(edge_start)||edge_start<0.||edge_start>=1.||
     !std::isfinite(p.offset)||std::abs(p.offset)>.25||
     !std::isfinite(p.plane_mix)||p.plane_mix<0.||p.plane_mix>1.||
     samples.size()!=chart.points.size()||!std::isfinite(chart.radius)||chart.radius<=0.||
     (p.mode==Mode::Adaptive&&!ms::validate(chart,filtered)))return false;
  const double alignment=dot(chart.axis,state.normal);
  if(!std::isfinite(alignment)||alignment<.5)return false;
  std::vector<double> staged;
  staged.reserve(samples.size());
  for(std::size_t i=0;i<samples.size();++i){
    const auto &q=chart.points[i];
    if(!finite(samples[i].p)||!std::isfinite(q.weight)||q.weight<0.||q.weight>1.)return false;
    const double signed_height=dot(samples[i].p-state.point,state.normal)/chart.radius;
    const double residual=(p.offset-signed_height)/alignment;
    const double direct=std::clamp(.85*residual,-.055,.055)*q.weight;
    double d=direct;
    if(p.mode==Mode::Adaptive){
      d=(1.-p.plane_mix)*filtered[i]+p.plane_mix*direct;
      // Neither adaptive component may drive across the target plane.
      d=std::clamp(d,std::min(0.,residual),std::max(0.,residual));
    }
    if(p.mode==Mode::Scrape)d=std::min(d,0.);
    if(p.mode==Mode::Fill)d=std::max(d,0.);
    // Additional geometric C2 taper, independent of pressure/mask/falloff.
    // Use full object-space brush distance, not only planar coordinates.
    const double r=std::sqrt(norm2(samples[i].p-chart.center))/chart.radius;
    d*=edge_envelope(r,edge_start);
    staged.push_back(d);
  }
  if(!ms::validate(chart,staged))return false;
  out=std::move(staged);return true;
}
} // namespace sculpt_physics::dedicated_d13::flatten
