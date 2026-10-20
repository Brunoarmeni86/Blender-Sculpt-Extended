/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace blender::bke::multires_layers::subdivide_work_v01 {

/* Bound concurrent grid working sets, never the total persistent geometry.
 * Oversized grids retain a serial path. No worker owns a whole layer Mesh. */
inline int lanes(const int grids,
                 const int area,
                 const int requested,
                 const int64_t working_bytes)
{
  if (grids <= 1 || area <= 0 || int64_t(grids) * area < 65536 || working_bytes <= 0) {
    return 1;
  }
  const int64_t per_grid = int64_t(area) * 36 + (int64_t(area) + 7) / 8 * 2 + 256;
  return int(std::max<int64_t>(1, std::min<int64_t>(
      std::min(grids, std::clamp(requested, 1, 8)), working_bytes / per_grid)));
}

/* No epsilon: every finite nonzero component, including subnormal values,
 * must remain allocated. NaN/Inf also cannot be classified as empty. */
inline bool exactly_zero(const float (*values)[3], const int count)
{
  if (values == nullptr || count < 0) {
    return false;
  }
  for (int i = 0; i < count; i++) {
    for (int axis = 0; axis < 3; axis++) {
      uint32_t bits;
      static_assert(sizeof(float) == sizeof(bits));
      std::memcpy(&bits, &values[i][axis], sizeof(bits));
      /* Integer bits avoid treating subnormals as zero under DAZ/FTZ modes. */
      if ((bits & 0x7fffffffu) != 0) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace blender::bke::multires_layers::subdivide_work_v01
