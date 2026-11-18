/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"

#include <vector>

namespace sculpt_physics {

/* Independent of a Multires modifier. A normal Sculpt mesh uses one-to-one
 * particle IDs; Multires uses barycentric proxy anchors plus detail residuals.
 * Blender/editor ownership and attributes remain outside the physics core. */
enum class SculptSurfaceKind { RegularMesh, MultiresSamples };

class SculptSurfaceBridge {
 public:
  /* Source vertex i is driven directly by physics particle i. */
  static SculptSurfaceBridge for_regular_mesh(const std::vector<Vec3> &vertex_positions,
                                              const std::vector<Particle> &proxy_particles);

  /* Anchored high-resolution sample positions use a low-resolution physics
   * proxy. Unmodeled high-frequency displacement is kept as a residual. */
  static SculptSurfaceBridge for_multires_samples(
      const std::vector<Vec3> &evaluated_positions,
      const std::vector<SurfaceAnchor> &anchors,
      const std::vector<Particle> &rest_proxy_particles);

  /* `standard_brush_positions` is an independent artistic brush result provided
   * by the caller; the bridge never invokes or replaces the ordinary brush.
   * No physics calls are needed in Standard / influence=0.
   * PhysicalAssist: linear blend standard <-> physical positions.
   * FullPhysics: physical positions.
   * The original mesh and UV layers are never modified here. */
  std::vector<Vec3> compose_positions(
      const std::vector<Particle> &current_proxy_particles,
      const std::vector<Vec3> &standard_brush_positions,
      BrushPhysicsSettings settings) const;

  SculptSurfaceKind kind() const { return kind_; }
  std::size_t sample_count() const { return sample_count_; }

 private:
  struct Sample {
    std::size_t particle_ids[3] = {};
    double weights[3] = {};
    Vec3 detail_residual{};
    // When valid, detail_residual stores rest-frame local coordinates rather
    // than an object-space vector, so no 3-vector rest frame is needed per sample.
    bool frame_valid = false;
  };
  SculptSurfaceKind kind_ = SculptSurfaceKind::RegularMesh;
  std::size_t sample_count_ = 0;
  std::vector<Sample> samples_;
};

}  // namespace sculpt_physics
