/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cmath>

namespace blender::bke::multires_layers::subdivide_scratch_v04 {

/* Each worker owns one private work grid. Keep the exact scalar add order of
 * the former serial loop; report failure without touching the source layer. */
template<typename Grid> bool prepare_grid(Grid &grid, const Grid *layer_grid)
{
  for (int i = 0; i < grid.totdisp; ++i) {
    for (int axis = 0; axis < 3; ++axis) {
      if (layer_grid && layer_grid->disps) {
        grid.disps[i][axis] += layer_grid->disps[i][axis];
      }
      if (!std::isfinite(grid.disps[i][axis])) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace blender::bke::multires_layers::subdivide_scratch_v04
