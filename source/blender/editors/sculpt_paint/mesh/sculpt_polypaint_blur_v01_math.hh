/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
namespace blender::ed::sculpt_paint::color::blur_v01_math {
/* RGB only. Alpha belongs to the source sample. No clamping of finite HDR colors. */
inline bool target(const float center[4],const double neighbor_sum[3],const int count,
                   const float strength,float result[4])
{
  if (count<=0 || !std::isfinite(strength) || strength<0 || strength>1) { return false; }
  for (int c=0;c<4;c++) { if (!std::isfinite(center[c])) { return false; } result[c]=center[c]; }
  for (int c=0;c<3;c++) {
    const double value=double(center[c])*(1.0-double(strength))+
        neighbor_sum[c]/count*double(strength);
    result[c]=float(value);
    if (!std::isfinite(value) || !std::isfinite(result[c])) { return false; }
  }
  return true;
}
}
