/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace sculpt_physics::smooth_recovery_d13 {
enum class Policy { Adaptive, Classic, Original, Extended, Invalid };
inline Policy parse(const char *s)
{
  if (!s || !*s || std::strcmp(s,"adaptive")==0) return Policy::Adaptive;
  if (std::strcmp(s,"classic")==0) return Policy::Classic;
  if (std::strcmp(s,"original")==0) return Policy::Original;
  if (std::strcmp(s,"extended")==0) return Policy::Extended;
  return Policy::Invalid;
}
inline const char *name(Policy p)
{
  switch(p) {
    case Policy::Classic: return "classic";
    case Policy::Original: return "original";
    case Policy::Extended: return "extended";
    case Policy::Adaptive: return "adaptive";
    default: return "invalid";
  }
}
struct Plan {int kernel_mode; int requested; int executed;};
inline Plan plan(Policy p,double edge_ratio,std::size_t active)
{
  if (p==Policy::Invalid || !std::isfinite(edge_ratio) || edge_ratio<0.)
    return {-1,0,0};
  const double r=std::max(1.,edge_ratio);
  const bool classic=p==Policy::Classic;
  const bool extended=p==Policy::Extended;
  const double coefficient=(classic||extended)?.010:.004;
  const int low=(classic||extended)?4:3;
  const int high=(classic||extended)?24:12;
  // Bound as double before conversion: no overflow for huge finite ratios.
  const int requested=int(std::clamp(std::ceil(r*r*coefficient),double(low),double(high)));
  const int cap=active>500000?4:active>200000?6:active>75000?8:12;
  return {classic?0:4,requested,p==Policy::Adaptive?std::min(requested,cap):requested};
}
} // namespace sculpt_physics::smooth_recovery_d13
