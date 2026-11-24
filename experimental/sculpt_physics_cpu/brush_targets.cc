/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "brush_targets.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sculpt_physics {
namespace {

bool finite(const Vec3 &v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double dot(const Vec3 &a, const Vec3 &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

double magnitude(const Vec3 &v)
{
  return std::hypot(v.x, v.y, v.z);
}

Vec3 unit(const Vec3 &v)
{
  const double length = magnitude(v);
  if (!std::isfinite(length) || length <= 1.0e-12) {
    throw std::invalid_argument("Invalid brush direction/normal");
  }
  return v / length;
}

}  // namespace

BrushAdjacency::BrushAdjacency(const World &world)
{
  neighbors_.resize(world.particles().size());
  for (const Bond &bond : world.bonds()) {
    if (bond.broken) continue;
    if (bond.a >= neighbors_.size() || bond.b >= neighbors_.size()) {
      throw std::invalid_argument("Out-of-range brush topology bond");
    }
    if (bond.a == bond.b) continue;
    neighbors_[bond.a].push_back(bond.b);
    neighbors_[bond.b].push_back(bond.a);
  }
  for (auto &neighbors : neighbors_) {
    std::sort(neighbors.begin(), neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
  }
}

const std::vector<std::size_t> &BrushAdjacency::neighbors(std::size_t index) const
{
  return neighbors_.at(index);
}

std::vector<World::BrushTargetInput> plan_assisted_brush(
    const World &world,
    const BrushAdjacency &topology,
    const std::vector<BrushVertexInput> &selection,
    const AssistedDab &dab,
    BrushPhysicsSettings settings)
{
  if (settings.mode == BrushPhysicsMode::Standard || settings.influence == 0.0) {
    return {};
  }
  if (!std::isfinite(settings.influence) || settings.influence < 0.0 ||
      settings.influence > 1.0 || !finite(dab.center) || !finite(dab.drag_delta) ||
      !std::isfinite(dab.radius) || dab.radius <= 0.0 ||
      !std::isfinite(dab.strength) || !std::isfinite(dab.compliance) ||
      dab.compliance < 0.0 || topology.size() != world.particles().size()) {
    throw std::invalid_argument("Invalid physical brush settings or topology");
  }
  const Vec3 brush_normal = (dab.kind == AssistedBrushKind::Clay ||
                              dab.kind == AssistedBrushKind::ClayStrips) ?
                                unit(dab.normal) : Vec3{};
  const Vec3 stroke_dir = dab.kind == AssistedBrushKind::ClayStrips ?
                              unit(dab.stroke_direction) : Vec3{};
  // Validate entire selection before returning even a single goal.
  std::vector<std::size_t> indices;
  indices.reserve(selection.size());
  for (const BrushVertexInput &sample : selection) {
    if (sample.particle_index >= world.particles().size() ||
        !std::isfinite(sample.factor) || sample.factor < 0.0 || sample.factor > 1.0 ||
        !finite(sample.normal)) {
      throw std::invalid_argument("Invalid PBVH brush sample");
    }
    indices.push_back(sample.particle_index);
  }
  std::sort(indices.begin(), indices.end());
  if (std::adjacent_find(indices.begin(), indices.end()) != indices.end()) {
    throw std::invalid_argument("Duplicate physical brush vertex");
  }

  std::vector<World::BrushTargetInput> goals;
  goals.reserve(selection.size());
  for (const BrushVertexInput &sample : selection) {
    const std::size_t i = sample.particle_index;
    const Particle &particle = world.particles()[i];
    if (particle.inverse_mass <= 0.0 || sample.factor == 0.0) continue;
    const Vec3 p = particle.position;
    if (!finite(p)) throw std::invalid_argument("Invalid proxy particle");
    const double influence = settings.mode == BrushPhysicsMode::FullPhysics ?
                                 settings.influence : 1.0;
    const double amount = sample.factor * dab.strength * influence;
    Vec3 displacement{};
    switch (dab.kind) {
      case AssistedBrushKind::Grab:
        displacement = dab.drag_delta * amount;
        break;
      case AssistedBrushKind::Clay:
        displacement = brush_normal * amount;
        break;
      case AssistedBrushKind::ClayStrips: {
        // Directional ribbon: maximum height at the centerline, tapering
        // smoothly with distance perpendicular to stroke direction.
        const Vec3 offset = p - dab.center;
        const Vec3 radial = offset - stroke_dir * dot(offset, stroke_dir);
        const double u = std::min(1.0, magnitude(radial) / dab.radius);
        const double ribbon = 1.0 - u * u * (3.0 - 2.0 * u);
        displacement = brush_normal * (amount * ribbon);
        break;
      }
      case AssistedBrushKind::Smooth: {
        const auto &neighbors = topology.neighbors(i);
        if (neighbors.empty()) continue;
        Vec3 sum{};
        for (std::size_t j : neighbors) sum += world.particles()[j].position;
        const Vec3 average = sum / static_cast<double>(neighbors.size());
        // Jacobi targets use the immutable world snapshot from this dab.
        displacement = (average - p) * std::max(-1.0, std::min(1.0, amount));
        break;
      }
      case AssistedBrushKind::GranularDrag:
        // Drag is a sculpt target. Local particle-particle contact resolution
        // is handled by granular_contacts.cc, not a fake "granular brush".
        displacement = dab.drag_delta * amount;
        break;
    }
    if (!finite(displacement)) {
      throw std::invalid_argument("Physical brush displacement overflow");
    }
    if (magnitude(displacement) <= 1.0e-15) continue;
    const Vec3 goal = p + displacement;
    if (!finite(goal)) throw std::invalid_argument("Physical brush target overflow");
    goals.push_back({i, goal, dab.compliance});
  }
  std::sort(goals.begin(), goals.end(), [](const auto &a, const auto &b) {
    return a.particle_index < b.particle_index;
  });
  return goals;
}

}  // namespace sculpt_physics
