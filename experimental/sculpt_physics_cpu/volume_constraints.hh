/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"

#include <array>
#include <cstddef>
#include <vector>

namespace sculpt_physics {

/* Standalone C++ reference for an XPBD signed-tetra-volume constraint.
 * The Blender mesh surface is not tetrahedral: a volumetric proxy/cage must
 * first be generated and mapped back to Sculpt/Multires surface samples.
 * This is independent source code based on the standard XPBD equations. */
struct TetraVolumeConstraint {
  std::array<std::size_t, 4> indices{};
  double rest_volume = 0.0;  // Signed, strictly positive; mesh caller owns winding.
  double compliance = 0.0;   // Compliance in units appropriate to volume^2.
  double lambda = 0.0;       // Iteration accumulator, transactional.
};

double signed_tetra_volume(const std::array<Vec3, 4> &positions);

/* Validates indices and volume, and constructs a constraint from the
 * undeformed proxy. Throws on flat/inverted rest tetrahedra. */
TetraVolumeConstraint make_volume_constraint(
    const std::vector<Vec3> &rest_positions,
    std::array<std::size_t, 4> indices,
    double compliance);

/* A standalone Jacobi-in-time / sequential-in-constraints CPU reference:
 * solve against a staged copy, preserving input positions and accumulated
 * lambdas on exceptions. No implicit Blender mesh/Undo writeback.
 * Future parallel backends must compare against this deterministic baseline. */
std::vector<Vec3> solve_tetra_volume_xpbd(
    const std::vector<Vec3> &positions,
    const std::vector<double> &inverse_masses,
    std::vector<TetraVolumeConstraint> &constraints,
    double dt,
    int iterations);

}  // namespace sculpt_physics
