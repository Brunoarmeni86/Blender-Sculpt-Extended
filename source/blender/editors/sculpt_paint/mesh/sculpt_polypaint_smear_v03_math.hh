/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
namespace blender::ed::sculpt_paint::color::smear_v03_math {
/* Pull color from upstream on the tangent plane. Two topological rings are
 * supplied by the caller; zero motion gives zero weight, never black paint. */
inline double weight(const std::array<double,3> &motion,const std::array<double,3> &edge,
                     const std::array<double,3> &normal)
{
  auto tangent=[&](std::array<double,3> v) {
    double n2=0,dot=0;
    for (int c=0;c<3;c++) { n2+=normal[c]*normal[c];dot+=v[c]*normal[c]; }
    if (!std::isfinite(n2) || n2<1e-30) { return std::array<double,3>{0,0,0}; }
    for (int c=0;c<3;c++) { v[c]-=normal[c]*(dot/n2); }
    return v;
  };
  const auto a=tangent(motion),b=tangent(edge);
  double a2=0,b2=0,dot=0;
  for (int c=0;c<3;c++) { a2+=a[c]*a[c];b2+=b[c]*b[c];dot+=a[c]*b[c]; }
  if (!std::isfinite(a2) || !std::isfinite(b2) || !std::isfinite(dot) || a2<1e-30 || b2<1e-30 || dot>=0) { return 0; }
  const double direction=std::min(1.0,-dot/std::sqrt(a2*b2));
  return direction*direction;
}
}
