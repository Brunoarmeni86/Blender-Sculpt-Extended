/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <unordered_map>

namespace blender::bke::multires_layers::detail {

/* IDs identify corners, not their current array positions. The caller additionally checks
 * cyclic face orientation before accepting a grid. Duplicate candidate IDs are counted so
 * that callers can reject all ambiguous copies instead of assigning one arbitrarily. */
inline bool corner_identity_lookup(const int *old_ids,
                                   const int old_num,
                                   const int *new_ids,
                                   const int new_num,
                                   int *new_to_old,
                                   int *old_use_count,
                                   int &max_old_id)
{
  if (old_num < 0 || new_num < 0 || (old_num > 0 && (!old_ids || !old_use_count)) ||
      (new_num > 0 && (!new_ids || !new_to_old)))
  {
    return false;
  }
  std::unordered_map<int, int> old_index;
  old_index.reserve(old_num);
  max_old_id = 0;
  for (int i = 0; i < old_num; i++) {
    if (old_ids[i] <= 0 || !old_index.emplace(old_ids[i], i).second) {
      return false;
    }
    max_old_id = std::max(max_old_id, old_ids[i]);
    old_use_count[i] = 0;
  }
  for (int i = 0; i < new_num; i++) {
    const auto found = old_index.find(new_ids[i]);
    new_to_old[i] = found == old_index.end() ? -1 : found->second;
    if (new_to_old[i] >= 0) {
      old_use_count[new_to_old[i]]++;
    }
  }
  return true;
}

}  // namespace blender::bke::multires_layers::detail

