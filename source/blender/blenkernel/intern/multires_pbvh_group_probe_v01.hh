/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>

namespace blender::bke::pbvh::group_probe_v01 {

inline int factor(const char *value)
{
  if (value && value[0] && value[1] == '\0' &&
      (value[0] == '2' || value[0] == '4'))
  {
    return value[0] - '0';
  }
  return 1;
}

inline int leaf_faces(const int area, const char *value)
{
  /* Diagnostic only: change the PBVH partition size, never the CCG sample count.
   * Apply the factor AFTER the native minimum so high levels can also be tested. */
  return area > 0 ? std::max(800 / area, 1) * factor(value) : 1;
}
}  // namespace blender::bke::pbvh::group_probe_v01
