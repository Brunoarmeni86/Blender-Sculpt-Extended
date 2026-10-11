/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cmath>
namespace blender::ed::sculpt_paint::brushes {
/* Level-1 CCG lattice: interpolate displacement weights, never replace high-res positions.
 * Probe checks are an admission heuristic, not a mathematical error bound.
 * Every grid boundary is evaluated exactly, even when adjacent admission differs. */
struct ElasticLevelOneField {
  float corners[4];
  float interpolate(int x, int y, int side) const {
    const float u = float(x) / float(side - 1);
    const float v = float(y) / float(side - 1);
    const float a = corners[0] * (1.0f-u) + corners[1] * u;
    const float b = corners[2] * (1.0f-u) + corners[3] * u;
    return a * (1.0f-v) + b * v;
  }
  template<typename Evaluate> bool prepare(int side, Evaluate evaluate, float tolerance) {
    if (side < 5 || !std::isfinite(tolerance) || tolerance <= 0.0f) { return false; }
    corners[0]=evaluate(0,0); corners[1]=evaluate(side-1,0);
    corners[2]=evaluate(0,side-1); corners[3]=evaluate(side-1,side-1);
    for (float c : corners) { if (!std::isfinite(c)) { return false; } }
    for (int j=1;j<8;j++) {
      for (int i=1;i<8;i++) {
        const int x=i*(side-1)/8, y=j*(side-1)/8;
        const float exact=evaluate(x,y), approximate=interpolate(x,y,side);
        if (!std::isfinite(exact) || std::fabs(exact-approximate)>tolerance) { return false; }
      }
    }
    return true;
  }
  static bool boundary(int x,int y,int side) {
    return x==0 || y==0 || x==side-1 || y==side-1;
  }
};
}
