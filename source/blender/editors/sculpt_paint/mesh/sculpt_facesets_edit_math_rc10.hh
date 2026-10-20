/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
/* One synchronous boundary step. No cascade within the operation; IDs are categorical.
 * Shrink chooses the lowest positive neighboring group, independent of iteration order. */
inline int facesets_rc10_boundary_id(const int current, const int active, const bool grow,
                                    const int *neighbors, const int count)
{
  int replacement = current;
  for (int i = 0; i < count; i++) {
    const int id = neighbors[i];
    if (grow && current != active && id == active) { return active; }
    if (!grow && current == active && id > 0 && id != active &&
        (replacement == active || id < replacement)) { replacement = id; }
  }
  return replacement;
}
