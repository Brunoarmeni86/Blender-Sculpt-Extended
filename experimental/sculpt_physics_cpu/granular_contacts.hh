/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"

#include <cstddef>
#include <vector>

namespace sculpt_physics {

/* Local sphere-grain contact projection (NOT a full DEM/MPM or fracture engine).
 * Spatial hash searches only supplied local PBVH/halo candidates. Existing
 * World bonds provide elastic/plastic/fracture behavior independently.
 * No geometry, UV or Sculpt Layer changes occur until caller stages targets. */
struct GranularContactSettings {
  int iterations = 4;
  double separation = 0.85; // Contact overlap correction per Jacobi iteration.
  double cohesion = 0.0; // Nonnegative short-range attraction, e.g. wet clay.
  double cohesion_range = 1.25; // Relative to combined particle radii.
  double compliance = 1.0e-6;
};

std::vector<World::BrushTargetInput> plan_granular_contacts(
    const World &world,
    const std::vector<std::size_t> &candidate_particles,
    const GranularContactSettings &settings);

}  // namespace sculpt_physics
