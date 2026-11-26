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
  if(name=="FIRM_CLAY" || name=="HARD_CLAY") return {.65,.45,.75,.80,.60,.90,.30,.85};
  if(name=="WAX") return {.55,.55,.55,.45,.85,.95,.50,.80};
  if(name=="MUD") return {.90,.10,.10,.10,1.0,.65,.85,.20};
  if(name=="FLESH") return {.62,.48,.58,.38,.48,.92,.28,.88};
  if(name=="FOAM") return {.35,.30,.22,.18,.20,.82,.18,.72};
  if(name=="STONE") return {.78,.72,.92,.96,.12,.98,.08,.96};
  if(name=="WOOD") return {.70,.58,.82,.86,.32,.94,.18,.90};
  if(name=="PUTTY") return {.88,.32,.72,.42,.98,.86,.72,.58};
  throw std::invalid_argument("Unsupported V25 surface material profile");
}

inline std::string_view surface_profile_name_v25(const int material)
{
  constexpr std::string_view names[] = {"SOFT_CLAY", "FIRM_CLAY", "MUD", "WAX", "FLESH",
                                        "FOAM", "STONE", "WOOD", "PUTTY"};
  return material >= 0 && material < 9 ? names[material] : names[0];
}

} // namespace sculpt_physics
