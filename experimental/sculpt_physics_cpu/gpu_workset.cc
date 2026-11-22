/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu_workset.hh"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace sculpt_physics {
namespace {
bool finite(Vec3 v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

std::vector<std::size_t> validated_set(
    const std::vector<std::size_t> &input, std::size_t bound)
{
  auto ids = input;
  for (std::size_t id : ids) {
    if (id >= bound) throw std::out_of_range("Invalid GPU proxy island ID");
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}
}

GpuLocalIsland build_gpu_local_island(
    const World &world,
    const std::vector<std::size_t> &active_ids,
    const std::vector<std::size_t> &halo_ids,
    const std::vector<std::size_t> &undo_covered_ids,
    std::uint64_t topology_generation,
    bool allow_truncated_graph)
{
  const auto &particles = world.particles();
  const auto active = validated_set(active_ids, particles.size());
  const auto halo = validated_set(halo_ids, particles.size());
  const auto covered = validated_set(undo_covered_ids, particles.size());
  for (std::size_t id : active) {
    if (!std::binary_search(covered.begin(), covered.end(), id)) {
      throw std::invalid_argument("Active GPU brush ID lacks Undo coverage");
    }
  }
  GpuLocalIsland island;
  island.topology_generation = topology_generation;
  island.global_ids.reserve(active.size() + halo.size());
  std::set_union(active.begin(), active.end(), halo.begin(), halo.end(),
                 std::back_inserter(island.global_ids));
  if (island.global_ids.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::length_error("GPU local proxy exceeds 32-bit indexing");
  }
  const std::size_t n = island.global_ids.size();
  island.positions.reserve(n);
  island.inverse_masses.reserve(n);
  island.radii.reserve(n);
  island.writable.reserve(n);
  std::unordered_map<std::size_t, std::uint32_t> local;
  local.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t id = island.global_ids[i];
    const Particle &p = particles[id];
    if (!finite(p.position) || !std::isfinite(p.inverse_mass) ||
        p.inverse_mass < 0.0 || !std::isfinite(p.radius) || p.radius <= 0.0) {
      throw std::invalid_argument("Invalid GPU proxy particle attributes");
    }
    const bool writeable = std::binary_search(covered.begin(), covered.end(), id);
    island.positions.push_back(p.position);
    island.inverse_masses.push_back(writeable ? p.inverse_mass : 0.0);
    island.radii.push_back(p.radius);
    island.writable.push_back(writeable);
    local.emplace(id, static_cast<std::uint32_t>(i));
  }
  for (const Bond &bond : world.bonds()) {
    if (bond.broken) continue;
    if (bond.a >= particles.size() || bond.b >= particles.size() ||
        !std::isfinite(bond.rest_length) || bond.rest_length < 0.0 ||
        !std::isfinite(bond.compliance) || bond.compliance < 0.0) {
      throw std::invalid_argument("Invalid GPU proxy bond");
    }
    const auto a = local.find(bond.a);
    const auto b = local.find(bond.b);
    if (a == local.end() && b == local.end()) continue;
    if (a == local.end() || b == local.end()) {
      ++island.crossing_bonds;
      continue;
    }
    island.bonds.push_back({a->second,b->second,bond.rest_length,bond.compliance});
  }
  if (island.crossing_bonds && !allow_truncated_graph) {
    throw std::logic_error("Incomplete GPU physics halo: bond crosses island boundary");
  }
  return island;
}

std::vector<GpuProxyChange> stage_gpu_local_writes(
    const GpuLocalIsland &island,
    const std::vector<Vec3> &solved_positions,
    double movement_epsilon)
{
  const std::size_t n = island.positions.size();
  if (solved_positions.size() != n || island.global_ids.size() != n ||
      island.writable.size() != n || island.inverse_masses.size() != n ||
      !std::isfinite(movement_epsilon) || movement_epsilon < 0.0) {
    throw std::invalid_argument("Invalid GPU writeback batch dimensions");
  }
  std::vector<GpuProxyChange> staged;
  staged.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (!finite(solved_positions[i]) || !finite(island.positions[i])) {
      throw std::invalid_argument("Non-finite GPU proxy writeback");
    }
    const Vec3 d = solved_positions[i] - island.positions[i];
    const double movement = std::hypot(d.x,d.y,d.z);
    if (!std::isfinite(movement)) {
      throw std::invalid_argument("GPU writeback distance overflow");
    }
    if (movement <= movement_epsilon) continue;
    if (!island.writable[i] || island.inverse_masses[i] <= 0.0) {
      throw std::logic_error("GPU modified pinned or non-Undo-covered vertex");
    }
    staged.push_back({island.global_ids[i],solved_positions[i]});
  }
  return staged;
}

}  // namespace sculpt_physics
