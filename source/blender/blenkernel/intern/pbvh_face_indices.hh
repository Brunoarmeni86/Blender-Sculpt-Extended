/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>

namespace blender::bke::pbvh {

/* Accepts Vector<int> or std::vector<int>. Keep sorted unique indices for
 * IndexMask::from_indices without allocating storage for untouched faces. */
template<typename Container> inline void sort_unique_face_indices(Container &indices)
{
  if (indices.size() < 2) {
    return;
  }
  std::sort(indices.begin(), indices.end());
  const auto last = std::unique(indices.begin(), indices.end());
  indices.resize(last - indices.begin());
}

}  // namespace blender::bke::pbvh
