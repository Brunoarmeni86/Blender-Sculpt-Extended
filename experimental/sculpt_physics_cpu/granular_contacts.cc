/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "granular_contacts.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace sculpt_physics {
namespace {

bool finite(Vec3 v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double length(Vec3 v)
{
  return std::hypot(v.x, v.y, v.z);
}

struct Cell {
  std::int64_t x, y, z;
  bool operator==(const Cell &b) const
  {
    return x == b.x && y == b.y && z == b.z;
  }
};

struct CellHash {
  std::size_t operator()(const Cell &p) const
  {
    const std::uint64_t x = static_cast<std::uint64_t>(p.x);
    const std::uint64_t y = static_cast<std::uint64_t>(p.y);
    const std::uint64_t z = static_cast<std::uint64_t>(p.z);
    return static_cast<std::size_t>((x * 0x9e3779b185ebca87ULL) ^
                                    (y * 0xc2b2ae3d27d4eb4fULL) ^
                                    (z * 0x165667b19e3779f9ULL));
  }
};

Cell cell(Vec3 p, double inverse_cell_size)
{
  // Enforce arithmetic safety before the float-to-int conversion.
  const auto coordinate = [inverse_cell_size](double component) -> std::int64_t {
    const double value = std::floor(component * inverse_cell_size);
    constexpr double limit = 9.0e15; // safely inside int64 and +/-27-cell traversal.
    if (!std::isfinite(value) || std::abs(value) > limit) {
      throw std::invalid_argument("Granular world bounds exceed spatial grid range");
    }
    return static_cast<std::int64_t>(value);
  };
  return {coordinate(p.x), coordinate(p.y), coordinate(p.z)};
}

}  // namespace

std::vector<World::BrushTargetInput> plan_granular_contacts(
    const World &world,
    const std::vector<std::size_t> &candidate_particles,
    const GranularContactSettings &settings)
{
  if (settings.iterations < 1 || settings.iterations > 64 ||
      !std::isfinite(settings.separation) || settings.separation < 0.0 ||
      settings.separation > 1.0 || !std::isfinite(settings.cohesion) ||
      settings.cohesion < 0.0 || settings.cohesion > 1.0 ||
      !std::isfinite(settings.cohesion_range) || settings.cohesion_range < 1.0 ||
      !std::isfinite(settings.compliance) || settings.compliance < 0.0) {
    throw std::invalid_argument("Invalid granular contact settings");
  }
  std::vector<std::size_t> ids = candidate_particles;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  const auto &particles = world.particles();
  // Reject invalid IDs even for a one-particle candidate list.
  for (std::size_t id : ids) {
    if (id >= particles.size()) throw std::out_of_range("Granular candidate OOB");
  }
  if (ids.size() < 2) return {};
  std::vector<Vec3> positions;
  positions.reserve(ids.size());
  double maximum_radius = 0.0;
  for (std::size_t id : ids) {
    if (id >= particles.size()) throw std::out_of_range("Granular candidate OOB");
    const Particle &p = particles[id];
    if (!finite(p.position) || !std::isfinite(p.radius) || p.radius <= 0.0 ||
        !std::isfinite(p.inverse_mass) || p.inverse_mass < 0.0) {
      throw std::invalid_argument("Invalid granular particle");
    }
    maximum_radius = std::max(maximum_radius, p.radius);
    positions.push_back(p.position);
  }
  const double cell_size = 2.0 * maximum_radius * settings.cohesion_range;
  if (!std::isfinite(cell_size) || cell_size <= 0.0) {
    throw std::invalid_argument("Invalid granular grid cell size");
  }
  const double inverse_cell_size = 1.0 / cell_size;
  if (!std::isfinite(inverse_cell_size)) throw std::invalid_argument("Granular cell too small");
  std::vector<Vec3> correction(ids.size());
  std::vector<Cell> cell_ids(ids.size());
  // Reuse the table's capacity across Jacobi iterations. Neighbor traversal
  // is local to the selected halo, not a full scan of the Sculpt mesh.
  std::unordered_map<Cell, std::vector<std::size_t>, CellHash> grid;
  grid.reserve(ids.size() * 2);
  for (int iteration = 0; iteration < settings.iterations; ++iteration) {
    grid.clear();
    std::fill(correction.begin(), correction.end(), Vec3{});
    for (std::size_t i = 0; i < ids.size(); ++i) {
      cell_ids[i] = cell(positions[i], inverse_cell_size);
      grid[cell_ids[i]].push_back(i);
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const Cell c = cell_ids[i];
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dz = -1; dz <= 1; ++dz) {
            const auto entry = grid.find({c.x + dx, c.y + dy, c.z + dz});
            if (entry == grid.end()) continue;
            for (std::size_t j : entry->second) {
              if (j <= i) continue;
              const Particle &a = particles[ids[i]];
              const Particle &b = particles[ids[j]];
              const double total_mass = a.inverse_mass + b.inverse_mass;
              if (total_mass <= 0.0) continue;
              const Vec3 offset = positions[j] - positions[i];
              const double distance = length(offset);
              const double touching = a.radius + b.radius;
              if (!std::isfinite(distance) || !std::isfinite(touching)) {
                throw std::invalid_argument("Granular contact overflow");
              }
              if (distance >= touching * settings.cohesion_range) continue;
              const Vec3 direction = distance > 1.0e-12 ?
                                         offset / distance : Vec3{1.0, 0.0, 0.0};
              const bool penetrating = distance < touching;
              const double magnitude = penetrating ?
                  (touching - distance) * settings.separation :
                  (distance - touching) * settings.cohesion;
              const Vec3 movement = direction * magnitude;
              const double sign = penetrating ? -1.0 : 1.0;
              correction[i] += movement * (sign * a.inverse_mass / total_mass);
              correction[j] += movement * (-sign * b.inverse_mass / total_mass);
            }
          }
        }
      }
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const Vec3 candidate = positions[i] + correction[i];
      if (!finite(candidate)) throw std::invalid_argument("Granular displacement overflow");
      positions[i] = candidate;
    }
  }
  std::vector<World::BrushTargetInput> targets;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (particles[ids[i]].inverse_mass <= 0.0) continue;
    if (length(positions[i] - particles[ids[i]].position) <= 1.0e-14) continue;
    targets.push_back({ids[i], positions[i], settings.compliance});
  }
  return targets;
}

}  // namespace sculpt_physics
