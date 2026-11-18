/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {
namespace {
constexpr double eps = 1e-12;
double length(Vec3 v) { return std::hypot(v.x, v.y, v.z); }
bool valid(double d) { return std::isfinite(d); }
}  // namespace

World::AdaptiveStepResult World::step_local_adaptive(double dt, int substeps,
                                                    int iterations, int initial_hops,
                                                    int max_hops, double boundary_fraction,
                                                    BoundaryPolicy policy)
{
  if (!active_stroke_) throw std::logic_error("begin_stroke required");
  if (!valid(dt) || dt <= 0.0 || substeps <= 0 || iterations <= 0 ||
      substeps > 256 || iterations > 256 || initial_hops < 0 ||
      max_hops < initial_hops || max_hops > 256 ||
      !valid(boundary_fraction) || boundary_fraction < 0.0) {
    throw std::invalid_argument("Invalid adaptive solver parameters");
  }

  // Global fallback is transactional too, though it requires a full snapshot.
  auto run_global = [&]() {
    // World::step is already transactional; avoid two extra full-world copies.
    step(dt, substeps, iterations);
    last_stats_.used_global_fallback = true;
  };

  if (targets_.empty() && !ground_enabled_ && length(gravity_) <= eps) {
    step_local(dt, substeps, iterations, initial_hops);
    return {true, initial_hops, 0, 0.0, 0.0, false};
  }
  if (ground_enabled_ || length(gravity_) > eps) {
    run_global();
    return {true, -1, 1, 0.0, 0.0, true};
  }

  // A BFS over unbroken bonds gives stable hop distances. The ordered queue is
  // a prefix for every candidate island; only affected states are snapshotted.
  std::vector<int> distance(particles_.size(), -1);
  std::vector<std::size_t> traversal;
  traversal.reserve(targets_.size());
  double max_target_displacement = 0.0;
  for (const GrabTarget &t : targets_) {
    if (t.index >= particles_.size()) throw std::logic_error("Stale Grab target");
    if (distance[t.index] < 0) {
      distance[t.index] = 0;
      traversal.push_back(t.index);
    }
    const Vec3 d = t.goal - particles_[t.index].position;
    max_target_displacement = std::max(max_target_displacement,
                                        std::hypot(d.x, d.y, d.z));
  }
  if (!valid(max_target_displacement)) throw std::invalid_argument("Invalid target span");
  for (std::size_t i = 0; i < traversal.size(); ++i) {
    const std::size_t pi = traversal[i];
    if (distance[pi] >= max_hops) continue;
    for (const std::size_t bi : particle_bond_ids_[pi]) {
      const Bond &b = bonds_[bi];
      if (b.broken) continue;
      const std::size_t other = b.a == pi ? b.b : b.a;
      if (distance[other] == -1) {
        distance[other] = distance[pi] + 1;
        traversal.push_back(other);
      }
    }
  }

  const auto original_dirty = dirty_;
  const auto original_movers = moving_particle_ids_;
  const StepStatistics original_stats = last_stats_;
  const auto original_targets = targets_;
  const double tolerance = std::max(1.0e-9, boundary_fraction * max_target_displacement);
  AdaptiveStepResult answer;
  // Within the same stroke, reuse the width previously required by the solver.
  int start_hops = std::min(max_hops,
                             std::max(initial_hops, adaptive_cached_hops_));
  // Account for motion retained by prior dabs, including across strokes:
  // reachable moving particles enlarge the candidate island; disconnected
  // or out-of-budget moving components require a transactional global step.
  const double minimum_speed = 1.0e-10 / dt;
  bool exterior_motion = false;
  for (const std::size_t i : moving_particle_ids_) {
    if (particles_[i].inverse_mass <= 0.0) continue;
    const Vec3 v = particles_[i].velocity;
    const double component_speed = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    if (!std::isfinite(component_speed) || component_speed <= minimum_speed) continue;
    if (distance[i] < 0 || distance[i] > max_hops) {
      exterior_motion = true;
      break;
    }
    start_hops = std::max(start_hops, distance[i]);
  }
  if (exterior_motion) {
    run_global();
    return {true, -1, 1, 0.0, 0.0, true};
  }
  for (int hops = start_hops;; hops = std::min(max_hops, std::max(hops + 1, hops * 2))) {
    ++answer.attempts;
    std::size_t n_active = 0;
    while (n_active < traversal.size() && distance[traversal[n_active]] <= hops) {
      ++n_active;
    }
    std::vector<Particle> before_particles;
    before_particles.reserve(n_active);
    std::vector<std::size_t> bond_ids;
    for (std::size_t j = 0; j < n_active; ++j) {
      const std::size_t id = traversal[j];
      before_particles.push_back(particles_[id]);
      bond_ids.insert(bond_ids.end(), particle_bond_ids_[id].begin(),
                      particle_bond_ids_[id].end());
    }
    std::sort(bond_ids.begin(), bond_ids.end());
    bond_ids.erase(std::unique(bond_ids.begin(), bond_ids.end()), bond_ids.end());
    std::vector<Bond> before_bonds;
    before_bonds.reserve(bond_ids.size());
    for (std::size_t id : bond_ids) before_bonds.push_back(bonds_[id]);

    auto restore = [&]() {
      for (std::size_t j = 0; j < n_active; ++j) {
        particles_[traversal[j]] = before_particles[j];
      }
      for (std::size_t j = 0; j < bond_ids.size(); ++j) {
        bonds_[bond_ids[j]] = before_bonds[j];
      }
      for (std::size_t j = 0; j < targets_.size(); ++j) {
        targets_[j] = original_targets[j];
      }
      dirty_ = original_dirty;
      moving_particle_ids_ = original_movers;
      last_stats_ = original_stats;
    };

    bool artificial_boundary = false;
    double max_boundary_motion = 0.0;
    double max_boundary_reaction = 0.0;
    try {
      step_local(dt, substeps, iterations, hops);
      // Only a movable shell attached to unsimulated movable particles is an
      // artificial interface. Actual fixed particles are valid physical pins.
      for (std::size_t j = 0; j < n_active; ++j) {
        const std::size_t pi = traversal[j];
        if (distance[pi] != hops) continue;
        bool touches_exterior = false;
        for (const std::size_t bi : particle_bond_ids_[pi]) {
          const Bond &b = bonds_[bi];
          if (b.broken) continue;
          const std::size_t other = b.a == pi ? b.b : b.a;
          if (distance[other] > hops || distance[other] < 0) {
            if (particles_[other].inverse_mass > 0.0) {
              touches_exterior = true;
              // A hard numerical boundary can cancel the brush without
              // moving. The XPBD lambda exposes the hidden reaction.
              const double reaction = std::abs(b.lambda) * particles_[pi].inverse_mass;
              max_boundary_reaction = std::max(max_boundary_reaction, reaction);
            }
          }
        }
        if (touches_exterior) {
          artificial_boundary = true;
          const Vec3 d = particles_[pi].position - before_particles[j].position;
          max_boundary_motion = std::max(max_boundary_motion,
                                           std::hypot(d.x, d.y, d.z));
        }
      }
      if (!valid(max_boundary_motion) || !valid(max_boundary_reaction)) {
        throw std::runtime_error("Invalid boundary displacement or reaction");
      }
    }
    catch (...) {
      restore();
      throw;
    }

    answer.max_boundary_motion = max_boundary_motion;
    answer.max_boundary_reaction = max_boundary_reaction;
    if (!artificial_boundary ||
        (max_boundary_motion <= tolerance && max_boundary_reaction <= tolerance)) {
      answer.accepted_hops = hops;
      adaptive_cached_hops_ = hops;
      return answer;
    }
    if (hops == max_hops && policy == BoundaryPolicy::BoundedApproximation) {
      // Fast but approximate: caller MUST inspect converged=false.
      answer.accepted_hops = hops;
      answer.converged = false;
      adaptive_cached_hops_ = hops;
      return answer;
    }
    restore();
    if (hops == max_hops) {
      // Accuracy fallback: the unconverged local result has been discarded.
      run_global();
      answer.accepted_hops = -1;
      adaptive_cached_hops_ = max_hops;
      answer.used_global_fallback = true;
      return answer;
    }
  }
}

}  // namespace sculpt_physics
