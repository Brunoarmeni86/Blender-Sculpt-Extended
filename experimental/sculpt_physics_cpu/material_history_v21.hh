/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physics_core.hh"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {

/* Reference material history on stable proxy cells. The geometry host must
 * commit/undo this state with its own geometry transaction, or not use it.
 * This deliberately has no global registry keyed by Blender pointers. */
struct MaterialCellState {
  double mass=0.0;
  double rest_volume=0.0;
  double accumulated_plastic_strain=0.0;
  double compaction=0.0;
};
struct MaterialHistory {
  std::uint64_t topology_revision=0;
  std::vector<MaterialCellState> cells;
};
struct MaterialHistoryEdit {
  std::size_t cell=0;
  double mass_delta=0.0;
  double volume_ratio=1.0;
  double plastic_increment=0.0;
};
inline MaterialHistory stage_material_history(
    const MaterialHistory &before,std::uint64_t topology_revision,
    const std::vector<MaterialHistoryEdit> &edits)
{
  if(before.topology_revision!=topology_revision)
    throw std::invalid_argument("Stale material topology");
  MaterialHistory after=before;
  std::vector<bool> seen(before.cells.size(),false);
  for(const auto &e:edits) {
    if(e.cell>=before.cells.size() || seen[e.cell] ||
       !std::isfinite(e.mass_delta) || !std::isfinite(e.volume_ratio) ||
       e.volume_ratio<=0 || !std::isfinite(e.plastic_increment) || e.plastic_increment<0)
      throw std::invalid_argument("Invalid material history edit");
    seen[e.cell]=true;
    auto &c=after.cells[e.cell];
    if(!std::isfinite(c.mass) || c.mass<0 ||
       !std::isfinite(c.rest_volume) || c.rest_volume<0 ||
       !std::isfinite(c.accumulated_plastic_strain) || c.accumulated_plastic_strain<0 ||
       !std::isfinite(c.compaction) || c.compaction<0 || c.compaction>1)
      throw std::invalid_argument("Invalid prior material state");
    c.mass+=e.mass_delta;
    c.rest_volume*=e.volume_ratio;
    c.accumulated_plastic_strain+=e.plastic_increment;
    c.compaction=std::clamp(c.compaction-std::log(e.volume_ratio),0.0,1.0);
    if(c.mass<0 || !std::isfinite(c.mass) || !std::isfinite(c.rest_volume) ||
       !std::isfinite(c.accumulated_plastic_strain))
      throw std::overflow_error("Invalid staged material state");
  }
  return after; // explicit host commit by move/swap; before remains unchanged
}

} // namespace sculpt_physics
