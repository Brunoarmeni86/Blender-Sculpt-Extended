/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"

#include <cstdint>
#include <memory>
#include <string_view>

namespace sculpt_physics {

enum class BackendKind {
  Auto,
  Cpu,
  WarpCuda,
};

enum class StrokeSolverPurpose {
  Grab,
  PhysicalClay,
  Smooth,
};

enum class SmoothSolverMode {
  Laplacian = 0,
  Taubin = 1,
  VolumePreserve = 2,
  Relax = 3,
  DenseStrong = 4,
};

BackendKind backend_kind_from_name(std::string_view name);

/* Brush-facing solver contract. Blender remains responsible for selection,
 * symmetry, masks, automasking and Undo. Backends receive only exact particle
 * targets and return positions; this keeps CPU and GPU paths behaviorally
 * comparable and prevents backend code from reaching editor state. */
class BrushSolverBackend {
 public:
  virtual ~BrushSolverBackend() = default;

  virtual BackendKind kind() const = 0;
  virtual void reserve_capacity(std::size_t particles, std::size_t bonds)
  {
    (void)particles;
    (void)bonds;
  }
  virtual bool synchronize_positions(const std::vector<Vec3> &positions)
  {
    (void)positions;
    return false;
  }
  /* Update only vertices changed by the standard brush before an optional
   * physical finish. This avoids an O(mesh size) upload for every dab. */
  virtual bool synchronize_positions_compact(const std::vector<std::size_t> &indices,
                                             const std::vector<Vec3> &positions,
                                             const bool update_rest = true)
  {
    (void)indices;
    (void)positions;
    (void)update_rest;
    return false;
  }
  virtual std::size_t add_particle(Vec3 position, double inverse_mass) = 0;
  virtual void add_bond(
      std::size_t a,
      std::size_t b,
      double compliance,
      double yield_strain = std::numeric_limits<double>::infinity(),
      double plastic_creep = 0.0,
      double fracture_strain = std::numeric_limits<double>::infinity()) = 0;
  virtual void set_velocity_damping(double damping_per_second) = 0;
  /* Negative keeps the backend's material-derived value. Zero disables
   * rest-shape retention, which is required by continuous Smooth. */
  virtual void set_shape_stiffness_override(double stiffness) = 0;
  /* Dedicated Smooth path. The active list is compact; topology neighbours
   * outside it are a read-only collar. Returns false on non-GPU backends. */
  virtual bool smooth_compact(const std::vector<std::size_t> &active,
                              const std::vector<float> &weights,
                              const std::vector<Vec3> &normals,
                              SmoothSolverMode mode,
                              double spatial_scale)
  {
    (void)active;
    (void)weights;
    (void)normals;
    (void)mode;
    (void)spatial_scale;
    return false;
  }
  virtual void begin_stroke() = 0;
  /* Restrict material response to particles already covered by the caller's
   * edit/Undo transaction. Targets are a subset of this region. */
  virtual bool set_active_region(const std::vector<std::size_t> &particles) = 0;
  virtual bool set_targets(const std::vector<World::BrushTargetInput> &targets) = 0;
  virtual void step_local(double dt, int substeps, int iterations, int bond_hops) = 0;
  virtual const std::vector<Particle> &particles() const = 0;
};

/* Auto intentionally selects the CPU reference until GPU parity is validated.
 * An unavailable explicit GPU request also falls back to CPU, so an optional
 * accelerator can never disable a brush. */
std::unique_ptr<BrushSolverBackend> create_brush_solver_backend(
    BackendKind requested, BackendKind *selected = nullptr);

/* One typed owner for experimental physical brush state. Blender keeps this
 * alive for one native Sculpt stroke. Brush adapters may replace it when the
 * purpose, topology generation or vertex count changes, but must never share a
 * solver between unrelated operations. This replaces independent opaque Grab
 * and Clay caches and is the A0 lifetime foundation for GPU-resident buffers. */
struct StrokeSolverSession {
  StrokeSolverPurpose purpose = StrokeSolverPurpose::Grab;
  BackendKind selected_backend = BackendKind::Cpu;
  std::size_t vertex_count = 0;
  /* Brush adapters use this to invalidate persistent GPU/CPU material state
   * when a preset or one of its physical parameters changes. */
  std::uint64_t configuration_key = 0;
  /* Topology scale captured when the persistent surface proxy is built.
   * Brush adapters use it to keep material response stable across mesh
   * densities without rescanning every edge on each dab. */
  double topology_mean_edge_length = 0.0;
  std::unique_ptr<BrushSolverBackend> solver;
  /* Brush-adapter scratch survives for one stroke. Generation stamps avoid
   * clearing an array the size of the complete mesh on every dab. */
  std::vector<int> compact_slot;
  std::vector<std::uint32_t> compact_stamp;
  std::uint32_t compact_generation = 0;
  std::vector<std::size_t> compact_indices;
  std::vector<float> compact_weights;
  std::vector<Vec3> compact_normals;
  std::vector<Vec3> compact_positions;
};

std::shared_ptr<StrokeSolverSession> create_stroke_solver_session(
    StrokeSolverPurpose purpose, BackendKind requested, std::size_t vertex_count);

}  // namespace sculpt_physics
