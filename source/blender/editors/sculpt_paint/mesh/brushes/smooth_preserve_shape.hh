/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cmath>
namespace blender::ed::sculpt_paint::brushes {
/* Reduce Laplacian motion along the normal by 80%, leave tangential motion unchanged.
 * This is directional attenuation, not a closed-surface volume constraint. */
inline void smooth_preserve_shape_translation(float translation[3], const float normal[3]) {
  const float length_squared=normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2];
  if (!std::isfinite(length_squared) || length_squared<=1.0e-20f) { return; }
  const float dot=translation[0]*normal[0]+translation[1]*normal[1]+translation[2]*normal[2];
  if (!std::isfinite(dot)) { return; }
  const float correction=0.8f*dot/length_squared;
  for(int axis=0;axis<3;axis++) { translation[axis]-=normal[axis]*correction; }
}
}
