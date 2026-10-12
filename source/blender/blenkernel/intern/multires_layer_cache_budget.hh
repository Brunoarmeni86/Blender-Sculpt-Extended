/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cstdint>

namespace blender::bke::multires_layers::cache_budget {

/* Each cache receives half of the optional total payload/lookup budget.
 * A negative budget selects the pre-existing allocation policy. */
inline int frame_capacity(const int requested,
                          const int area,
                          const int grids_num,
                          const int64_t budget,
                          const int frame_bytes)
{
  if (budget < 0) {
    return requested;
  }
  if (area <= 0 || grids_num < 0 || frame_bytes <= 0) {
    return 0;
  }
  const int64_t lookup_bytes = int64_t(grids_num) * sizeof(int);
  const int64_t available = budget / 2 - lookup_bytes;
  const int64_t per_grid = int64_t(area) * frame_bytes + sizeof(int) + sizeof(uint8_t);
  if (available < per_grid) {
    return 0;
  }
  return int(std::min<int64_t>(std::clamp(requested, 0, grids_num), available / per_grid));
}

inline bool weight_fits(const int payload_grids,
                        const int area,
                        const int grids_num,
                        const int64_t budget)
{
  if (budget < 0) {
    return true;
  }
  if (payload_grids < 0 || area <= 0 || grids_num < 0) {
    return false;
  }
  const int64_t bytes = int64_t(payload_grids) * area * 3 * sizeof(float) +
                        int64_t(grids_num) * sizeof(int) + int64_t(payload_grids) * sizeof(int);
  return bytes <= budget / 2;
}
}  // namespace blender::bke::multires_layers::cache_budget
