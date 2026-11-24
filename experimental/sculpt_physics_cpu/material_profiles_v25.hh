/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "clay_surface_solver.hh"
#include <string_view>

namespace sculpt_physics {

/* Qualitative controls for the fixed-topology surface backend.
 * Not physical units, measured rheology, or a fluid/elastic solver.
 * Volume fraction is the return fraction of Compact; other conservative
 * tools retain their signed projected-volume budget. */
struct SurfaceMaterialProfileV25 {
  double friction,viscosity,cohesion,stiffness,plasticity;
  double volume_preservation,fusion,detail_retention;
};
inline SurfaceMaterialProfileV25 surface_profile_v25(std::string_view name)
{
  if(name=="SOFT_CLAY") return {.85,.20,.35,.25,.95,.75,.60,.65};
  if(name=="HARD_CLAY") return {.65,.45,.75,.80,.60,.90,.30,.85};
  if(name=="WAX") return {.55,.55,.55,.45,.85,.95,.50,.80};
  if(name=="MUD") return {.90,.10,.10,.10,1.0,.65,.85,.20};
  throw std::invalid_argument("Unsupported V25 surface material profile");
}

} // namespace sculpt_physics
