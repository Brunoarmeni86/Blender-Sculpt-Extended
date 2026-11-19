/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <limits>
#include <vector>

namespace sculpt_physics {

struct Vec3 {
  double x = 0.0, y = 0.0, z = 0.0;
  Vec3 operator+(const Vec3 &other) const;
  Vec3 operator-(const Vec3 &other) const;
  Vec3 operator*(double scale) const;
  Vec3 operator/(double scale) const;
  Vec3 &operator+=(const Vec3 &other);
};

struct UV2 {
  double u = 0.0, v = 0.0;
};

struct Particle {
  Vec3 position;
  Vec3 velocity;
  double inverse_mass = 1.0;
  double radius = 0.05;
};

struct Bond {
  std::size_t a = 0, b = 0;
  double rest_length = 0.0;
  double compliance = 0.0;       // m / N, XPBD compliance
  double yield_strain = std::numeric_limits<double>::infinity();
  double plastic_creep = 0.0;    // 1/s, when beyond yield strain
  double fracture_strain = std::numeric_limits<double>::infinity();
  double lambda = 0.0;
  bool broken = false;
};

enum class BrushPhysicsMode { Standard, PhysicalAssist, FullPhysics };

struct BrushPhysicsSettings {
  BrushPhysicsMode mode = BrushPhysicsMode::Standard;
  // Assist: blend factor for standard vs physical surface result (applied once).\n  // FullPhysics: physical Grab-target strength. Zero bypasses the solver.\n  double influence = 0.0;
  double compliance = 1e-6;
};

/* Stable proxy-to-surface reference. UVs belong to face corners (not particle IDs),
 * so two UV islands may reference the same vertices without collapsing a seam. */
struct SurfaceAnchor {
  std::size_t vertices[3] = {0, 0, 0};
  double weights[3] = {1.0, 0.0, 0.0};
  UV2 corner_uv[3] = {};
};

struct SurfaceSample {
  Vec3 position;
  UV2 uv;
};

SurfaceSample evaluate_anchor(const SurfaceAnchor &anchor,
                              const std::vector<Particle> &particles);

class World {
 public:
  std::size_t add_particle(Vec3 position, double inverse_mass = 1.0,
                           double radius = 0.05);
  std::size_t add_bond(std::size_t a, std::size_t b, double compliance = 0.0,
                       double yield_strain = std::numeric_limits<double>::infinity(),
                       double plastic_creep = 0.0,
                       double fracture_strain = std::numeric_limits<double>::infinity());
  void set_gravity(Vec3 gravity);
  void set_velocity_damping(double damping_per_second);
  void set_ground_height(double y); // Disabled by default.
  void disable_ground();

  /* Transactional stroke protocol: begin -> apply targets -> step -> commit/cancel.
   * Only one active stroke per world; cancel restores particles AND bond state. */
  void begin_stroke();
  bool apply_grab(Vec3 center, double radius, Vec3 displacement,
                  BrushPhysicsSettings settings);
  /* Accept a PBVH-selected candidate set: avoids scanning all physics proxy
   * particles for a brush with a small footprint. Caller supplies all nearby
   * proxy IDs; duplicates and order are normalized, invalid IDs throw without
   * replacing the previous valid dab. C++17 reference, Blender Span adapter TBD. */
  bool apply_grab_candidates(Vec3 center, double radius, Vec3 displacement,
                             BrushPhysicsSettings settings,
                             const std::vector<std::size_t> &candidate_ids);
  void step(double dt, int substeps = 4, int iterations = 5);
  /* Optional approximation for brush-local, quasistatic strokes. Only particles
   * within `bond_hops` from a Grab target are integrated. Crossing bonds act as
   * fixed boundary constraints. Gravity/ground fall back to the global step.
   * Not an exact replacement for a globally simulated dynamic material. */
  void step_local(double dt, int substeps = 4, int iterations = 5,
                  int bond_hops = 4);
  /* Experimental multi-island quasistatic path: integrate Grab targets
   * plus separately moving particles from earlier dabs, with the same
   * approximate bond-hop halo as step_local. Gravity/ground stay global.
   * Default step_local and adaptive behavior are unchanged. */
  void step_local_multi(double dt, int substeps = 4, int iterations = 5,
                        int bond_hops = 4);
  struct StepStatistics {
    std::size_t integrated_particles = 0;
    std::size_t visited_bonds = 0;
    bool used_global_fallback = false;
  };
  /* Experimental local solver: expand the active region when artificial
   * boundary motion exceeds the specified fraction of the brush motion.
   * Attempts are transactional; strict mode falls back to global XPBD, whereas
   * bounded mode explicitly reports an unconverged approximation. */
  enum class BoundaryPolicy { StrictGlobalFallback, BoundedApproximation };
  struct AdaptiveStepResult {
    bool converged = true; // False: budget reached with unresolved boundary motion.
    int accepted_hops = 0; // -1 on global fallback.
    int attempts = 0;
    double max_boundary_motion = 0.0;
    double max_boundary_reaction = 0.0; // Reaction from numerically pinned exterior.
    bool used_global_fallback = false;
  };
  AdaptiveStepResult step_local_adaptive(double dt, int substeps = 4,
                                         int iterations = 5,
                                         int initial_hops = 4,
                                         int max_hops = 16,
                                         double boundary_fraction = 0.02,
                                         BoundaryPolicy policy =
                                             BoundaryPolicy::StrictGlobalFallback);
  StepStatistics last_step_statistics() const;
  /* Original physical state recorded once per stroke, for undo/cancel.
   * Sparse for a small brush island, upgraded to a contiguous snapshot only
   * if full-world simulation is required. Payload bytes omit map overhead. */
  struct StrokeJournalStatistics {
    std::size_t particles_recorded = 0;
    std::size_t bonds_recorded = 0;
    std::size_t approximate_payload_bytes = 0;
    bool dense_fallback = false;
  };
  StrokeJournalStatistics stroke_journal_statistics() const;
  void commit_stroke();
  void cancel_stroke();
  bool stroke_active() const;

  const std::vector<Particle> &particles() const;
  const std::vector<Bond> &bonds() const;
  const std::vector<std::size_t> &dirty_particles() const;

 private:
  bool apply_grab_impl(Vec3 center, double radius, Vec3 displacement,
                       BrushPhysicsSettings settings,
                       const std::vector<std::size_t> *candidate_ids);
  void step_local_impl(double dt, int substeps, int iterations, int bond_hops,
                       bool include_moving_islands);
  struct GrabTarget {
    std::size_t index;
    Vec3 goal;
    double compliance;
    double lambda_x = 0, lambda_y = 0, lambda_z = 0;
  };

  std::vector<Particle> particles_;
  std::vector<Bond> bonds_;
  /* Undirected bond incidence, built once as topology is added. */
  std::vector<std::vector<std::size_t>> particle_bond_ids_;
  StepStatistics last_stats_{};
  int adaptive_cached_hops_ = -1; // Reuse tested halo width within one stroke.
  std::vector<GrabTarget> targets_;
  std::vector<GrabTarget> pending_targets_; // Reused scratch for atomic Grab updates.
  std::vector<std::size_t> brush_candidate_scratch_; // Sort/deduplicate PBVH IDs.
  std::vector<std::size_t> dirty_;
  // Stable indices: particle/bond topology cannot change during a stroke.
  std::unordered_map<std::size_t, Particle> undo_particles_;
  std::unordered_map<std::size_t, Bond> undo_bonds_;
  bool dense_journal_ = false;
  std::vector<Particle> dense_undo_particles_;
  std::vector<Bond> dense_undo_bonds_;
  // Persistent scratch avoids O(N) stamp clearing and repeated allocation
  // on ordinary Sculpt / Multires proxy brush dabs. The World is non-reentrant.
  std::vector<std::uint32_t> included_generation_marks_;
  std::uint32_t included_generation_ = 0;
  std::vector<std::size_t> local_active_scratch_;
  std::vector<std::size_t> local_bond_scratch_;
  std::vector<Vec3> local_start_scratch_;
  std::vector<Vec3> local_previous_scratch_;
  // Reused journal key lists: a rejected dab must remove only its new entries.
  std::vector<std::size_t> local_undo_new_particle_ids_;
  std::vector<std::size_t> local_undo_new_bond_ids_;
  // Sparse list of particles retaining velocity across dabs/strokes.
  std::vector<std::size_t> moving_particle_ids_;
  std::vector<std::size_t> saved_moving_particle_ids_;
  bool active_stroke_ = false;
  bool ground_enabled_ = false;
  double ground_y_ = 0.0;
  double damping_ = 0.0;
  Vec3 gravity_{};
  // Config edits within an active stroke belong to its undo transaction.
  bool saved_ground_enabled_ = false;
  double saved_ground_y_ = 0.0;
  double saved_damping_ = 0.0;
  Vec3 saved_gravity_{};

};

}  // namespace sculpt_physics
