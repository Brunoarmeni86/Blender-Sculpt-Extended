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
} // namespace sculpt_physics::dedicated_d13::flatten
