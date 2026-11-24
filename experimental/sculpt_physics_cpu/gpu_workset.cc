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

static GpuLocalIsland build_gpu_local_island_impl(
    const World &world,
    const std::vector<std::size_t> &active_ids,
    const std::vector<std::size_t> &halo_ids,
    const std::vector<std::size_t> &undo_covered_ids,
    std::uint64_t topology_generation,
    bool allow_truncated_graph,
    const std::vector<std::size_t> *candidate_bond_ids)
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
  const auto &bonds = world.bonds();
  const std::size_t scan_count = candidate_bond_ids ? candidate_bond_ids->size() : bonds.size();
  for (std::size_t scan_index = 0; scan_index < scan_count; ++scan_index) {
    const std::size_t bond_index = candidate_bond_ids ?
                                        (*candidate_bond_ids)[scan_index] : scan_index;
    if (bond_index >= bonds.size()) {
      throw std::out_of_range("Stale GPU proxy bond index");
    }
    const Bond &bond = bonds[bond_index];
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
      // Exactly one endpoint is included in the island. A fixed local
      // endpoint cannot move due to this bond, so it does not exert any
      // effect on the editable interior and we may safely omit it.
      const auto inside = a != local.end() ? a : b;
      if (island.inverse_masses[inside->second] == 0.0) {
        ++island.ignored_fixed_crossings;
      }
      else {
        ++island.crossing_bonds;
      }
      continue;
    }
    island.bonds.push_back({a->second,b->second,bond.rest_length,bond.compliance});
  }
  if (island.crossing_bonds && !allow_truncated_graph) {
    throw std::logic_error("Incomplete GPU physics halo: bond crosses island boundary");
  }
  return island;
}

GpuLocalIsland build_gpu_local_island(
    const World &world,
    const std::vector<std::size_t> &active_ids,
    const std::vector<std::size_t> &halo_ids,
    const std::vector<std::size_t> &undo_covered_ids,
    std::uint64_t topology_generation,
    bool allow_truncated_graph)
{
  return build_gpu_local_island_impl(world, active_ids, halo_ids,
                                     undo_covered_ids, topology_generation,
                                     allow_truncated_graph, nullptr);
}

GpuTopologyBondIndex::GpuTopologyBondIndex(const World &world,
                                           std::uint64_t topology_generation)
    : world_(&world),
      generation_(topology_generation),
      particle_count_(world.particles().size()),
      bond_count_(world.bonds().size())
{
  // World::add_bond maintains authoritative undirected incidence in
  // insertion order. Reuse it rather than allocating a second O(V+E) graph
  // at the beginning of every Blender stroke. The generation and count
  // checks below still guard against invalidated topology.
}

GpuLocalIsland GpuTopologyBondIndex::build(
    const World &world,
    const std::vector<std::size_t> &active_ids,
    const std::vector<std::size_t> &halo_ids,
    const std::vector<std::size_t> &undo_covered_ids,
    std::uint64_t topology_generation,
    bool allow_truncated_graph) const
{
  // Explicit caller generation is the authoritative topology ID.
  // Stable counts alone cannot detect reordering or in-place edits.
  if (world_ != &world || topology_generation != generation_ ||
      world.particles().size() != particle_count_ ||
      world.bonds().size() != bond_count_) {
    throw std::logic_error("Stale GPU topology index: rebuild after topology change");
  }
  const auto active = validated_set(active_ids, particle_count_);
  const auto halo = validated_set(halo_ids, particle_count_);
  std::vector<std::size_t> included;
  included.reserve(active.size() + halo.size());
  std::set_union(active.begin(), active.end(), halo.begin(), halo.end(),
                 std::back_inserter(included));

  // O(sum of local vertex degrees) rather than scanning every bond in the
  // full Sculpt mesh. Sorting retains original stable bond insertion order.
  std::vector<std::size_t> bond_ids;
  for (std::size_t id : included) {
    const auto &incident = world.incident_bond_ids(id);
    bond_ids.insert(bond_ids.end(), incident.begin(), incident.end());
  }
  std::sort(bond_ids.begin(), bond_ids.end());
  bond_ids.erase(std::unique(bond_ids.begin(), bond_ids.end()), bond_ids.end());
  return build_gpu_local_island_impl(world, active_ids, halo_ids,
                                     undo_covered_ids, topology_generation,
                                     allow_truncated_graph, &bond_ids);
}

std::vector<std::size_t> GpuTopologyBondIndex::one_ring_halo(
    const World &world,
    const std::vector<std::size_t> &active_ids,
    std::uint64_t topology_generation) const
{
  if (world_ != &world || topology_generation != generation_ ||
      world.particles().size() != particle_count_ ||
      world.bonds().size() != bond_count_) {
    throw std::logic_error("Stale GPU topology index: rebuild halo after topology change");
  }
  const auto active = validated_set(active_ids, particle_count_);
  std::vector<std::size_t> halo;
  for (const std::size_t id : active) {
    for (const std::size_t bond_id : world.incident_bond_ids(id)) {
      const Bond &bond = world.bonds()[bond_id];
      if (bond.broken) continue;
      const std::size_t neighbor = bond.a == id ? bond.b : bond.a;
      if (!std::binary_search(active.begin(), active.end(), neighbor)) {
        halo.push_back(neighbor);
      }
    }
  }
  std::sort(halo.begin(), halo.end());
  halo.erase(std::unique(halo.begin(), halo.end()), halo.end());
  return halo;
}

GpuLocalFloatPack pack_gpu_island_relative(const GpuLocalIsland &island)
{
  static_assert(sizeof(GpuFloat3) == 3 * sizeof(float),
                "GpuFloat3 must be packed as three float32 values");
  GpuLocalFloatPack packed;
  if (island.positions.empty()) return packed;

  Vec3 lo = island.positions.front();
  Vec3 hi = lo;
  for (const Vec3 &p : island.positions) {
    if (!finite(p)) throw std::invalid_argument("Non-finite GPU proxy position");
    lo.x = std::min(lo.x,p.x); lo.y = std::min(lo.y,p.y); lo.z = std::min(lo.z,p.z);
    hi.x = std::max(hi.x,p.x); hi.y = std::max(hi.y,p.y); hi.z = std::max(hi.z,p.z);
  }
  // Half-sums resist overflow when the original coordinate magnitude is
  // enormous. A local origin is fixed for this whole device-island lifetime.
  packed.origin = {0.5*lo.x + 0.5*hi.x,
                   0.5*lo.y + 0.5*hi.y,
                   0.5*lo.z + 0.5*hi.z};
  if (!finite(packed.origin)) {
    throw std::invalid_argument("Non-finite GPU island origin");
  }
  packed.relative_positions.reserve(island.positions.size());
  for (const Vec3 &p : island.positions) {
    const Vec3 offset = p - packed.origin;
    if (!finite(offset)) throw std::invalid_argument("GPU island span overflow");
    const GpuFloat3 local{static_cast<float>(offset.x),
                          static_cast<float>(offset.y),
                          static_cast<float>(offset.z)};
    if (!std::isfinite(local.x) || !std::isfinite(local.y) ||
        !std::isfinite(local.z)) {
      throw std::invalid_argument("GPU island span exceeds float32 range");
    }
    packed.relative_positions.push_back(local);
  }
  return packed;
}

std::vector<GpuProxyChange> stage_gpu_relative_writes(
    const GpuLocalIsland &island,
    const GpuLocalFloatPack &packed,
    const std::vector<GpuFloat3> &solved_relative,
    double movement_epsilon)
{
  const std::size_t n = island.positions.size();
  if (solved_relative.size() != n || packed.relative_positions.size() != n ||
      island.global_ids.size() != n || island.writable.size() != n ||
      island.inverse_masses.size() != n || !finite(packed.origin) ||
      !std::isfinite(movement_epsilon) || movement_epsilon < 0.0) {
    throw std::invalid_argument("Invalid rebased GPU writeback dimensions");
  }
  std::vector<GpuProxyChange> changes;
  changes.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const GpuFloat3 cur = solved_relative[i];
    const GpuFloat3 old = packed.relative_positions[i];
    if (!std::isfinite(cur.x) || !std::isfinite(cur.y) || !std::isfinite(cur.z) ||
        !std::isfinite(old.x) || !std::isfinite(old.y) || !std::isfinite(old.z)) {
      throw std::invalid_argument("Non-finite relative GPU result");
    }
    const Vec3 d{static_cast<double>(cur.x) - static_cast<double>(old.x),
                 static_cast<double>(cur.y) - static_cast<double>(old.y),
                 static_cast<double>(cur.z) - static_cast<double>(old.z)};
    const double displacement = std::hypot(d.x,d.y,d.z);
    if (!std::isfinite(displacement)) throw std::invalid_argument("GPU delta overflow");
    if (displacement <= movement_epsilon) continue;
    if (!island.writable[i] || island.inverse_masses[i] <= 0.0) {
      throw std::logic_error("GPU changed pinned vertex in rebased island");
    }
    const Vec3 world{packed.origin.x + static_cast<double>(cur.x),
                     packed.origin.y + static_cast<double>(cur.y),
                     packed.origin.z + static_cast<double>(cur.z)};
    if (!finite(world)) throw std::invalid_argument("GPU world-space result overflow");
    const Vec3 change = world - island.positions[i];
    if (std::hypot(change.x,change.y,change.z) > movement_epsilon) {
      changes.push_back({island.global_ids[i],world});
    }
  }
  return changes;
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
