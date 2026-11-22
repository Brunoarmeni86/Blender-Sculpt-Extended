/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sculpt_physics {

/* Compact GPU island: one-time host gather for a stable local topology.
 * This is a C++ transfer contract, NOT a CUDA or Warp runtime implementation.
 * Local IDs match the future GPU proxy arrays; global_ids refer to Blender's
 * authoritative persistent proxy. Only undo_covered IDs are writable. */
struct GpuLocalBond {
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  double rest_length = 0.0;
  double compliance = 0.0;
};

struct GpuLocalIsland {
  std::uint64_t topology_generation = 0;
  std::vector<std::size_t> global_ids;  // sorted, unique global proxy IDs
  std::vector<Vec3> positions;
  std::vector<double> inverse_masses;  // zero = pinned exterior
  std::vector<double> radii;
  std::vector<GpuLocalBond> bonds;
  std::vector<bool> writable;
  std::size_t crossing_bonds = 0; // truncated local graph, not exact physics
};

struct GpuProxyChange {
  std::size_t global_index = 0;
  Vec3 position{};
};

/* Active IDs must all be Undo-covered. Halo IDs may extend beyond the Undo
 * scope, but are *pinned* in the GPU island. Crossing graph bonds normally
 * mean the workset is incomplete. Explicit allow_truncated_graph is required
 * for approximate local behavior; prefer adaptive-halo expansion instead. */
GpuLocalIsland build_gpu_local_island(
    const World &world,
    const std::vector<std::size_t> &active_ids,
    const std::vector<std::size_t> &halo_ids,
    const std::vector<std::size_t> &undo_covered_ids,
    std::uint64_t topology_generation,
    bool allow_truncated_graph = false);

/* Validate staged GPU positions before ANY Blender PBVH/CCG writeback.
 * Pinned/halo vertices must not move. Returns only changed writable global
 * positions; caller must also validate topology generation and CCG anchors. */
std::vector<GpuProxyChange> stage_gpu_local_writes(
    const GpuLocalIsland &island,
    const std::vector<Vec3> &solved_positions,
    double movement_epsilon = 1.0e-12);

}  // namespace sculpt_physics
