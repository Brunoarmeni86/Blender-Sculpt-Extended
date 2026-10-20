/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace blender::draw::overlay_neutral_v01 {

/* Mask classification follows PBVH's maintained FullyUnmasked metadata.
 * High-resolution Face Sets are deliberately kept on the full draw path.
 * A node can be omitted only when BOTH displayed effects are neutral. */
template<typename GridRange, typename FaceSetForGrid>
bool needs_overlay(const bool show_mask,
                   const bool has_masks,
                   const bool fully_unmasked,
                   const bool show_face_sets,
                   const bool has_high_res_face_sets,
                   const bool has_face_sets,
                   const int default_face_set,
                   const GridRange &grids,
                   FaceSetForGrid face_set_for_grid)
{
  if (show_mask && has_masks && !fully_unmasked) {
    return true;
  }
  if (show_face_sets) {
    if (has_high_res_face_sets) {
      return true;
    }
    if (has_face_sets) {
      for (const int grid : grids) {
        if (face_set_for_grid(grid) != default_face_set) {
          return true;
        }
      }
    }
  }
  return false;
}
}  // namespace blender::draw::overlay_neutral_v01
