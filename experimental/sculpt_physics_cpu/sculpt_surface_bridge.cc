/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sculpt_surface_bridge.hh"

#include <cmath>
#include <stdexcept>

namespace sculpt_physics {
namespace {
bool finite(Vec3 v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
Vec3 anchored_position(const SurfaceAnchor &a, const std::vector<Particle> &particles)
{
  Vec3 p{};
  double sum = 0.0;
  for (int i = 0; i < 3; i++) {
    if (a.vertices[i] >= particles.size() || !std::isfinite(a.weights[i]) ||
        a.weights[i] < 0.0) {
      throw std::invalid_argument("Invalid Multires proxy anchor");
    }
    p += particles[a.vertices[i]].position * a.weights[i];
    sum += a.weights[i];
  }
  if (std::abs(sum - 1.0) > 1e-6) {
    throw std::invalid_argument("Invalid anchor weight sum");
  }
  return p;
}
}  // namespace

SculptSurfaceBridge SculptSurfaceBridge::for_regular_mesh(
    const std::vector<Vec3> &vertex_positions,
    const std::vector<Particle> &proxy_particles)
{
  if (vertex_positions.size() != proxy_particles.size()) {
    throw std::invalid_argument("Regular Sculpt requires one proxy particle per vertex");
  }
  SculptSurfaceBridge result;
  result.kind_ = SculptSurfaceKind::RegularMesh;
  result.source_positions_ = vertex_positions;
  result.samples_.reserve(vertex_positions.size());
  for (std::size_t i = 0; i < vertex_positions.size(); ++i) {
    if (!finite(vertex_positions[i]) || !finite(proxy_particles[i].position)) {
      throw std::invalid_argument("Invalid regular mesh position");
    }
    Sample sample;
    sample.particle_ids[0] = i;
    sample.weights[0] = 1.0;
    result.samples_.push_back(sample);
  }
  return result;
}

SculptSurfaceBridge SculptSurfaceBridge::for_multires_samples(
    const std::vector<Vec3> &evaluated_positions,
    const std::vector<SurfaceAnchor> &anchors,
    const std::vector<Particle> &rest_proxy_particles)
{
  if (evaluated_positions.size() != anchors.size()) {
    throw std::invalid_argument("Evaluated Multires positions/anchors size mismatch");
  }
  SculptSurfaceBridge result;
  result.kind_ = SculptSurfaceKind::MultiresSamples;
  result.source_positions_ = evaluated_positions;
  result.samples_.reserve(anchors.size());
  for (std::size_t i = 0; i < anchors.size(); ++i) {
    if (!finite(evaluated_positions[i])) {
      throw std::invalid_argument("Invalid evaluated Multires position");
    }
    Sample sample;
    const SurfaceAnchor &anchor = anchors[i];
    const Vec3 rest = anchored_position(anchor, rest_proxy_particles);
    for (int k = 0; k < 3; ++k) {
      sample.particle_ids[k] = anchor.vertices[k];
      sample.weights[k] = anchor.weights[k];
    }
    sample.detail_residual = evaluated_positions[i] - rest;
    result.samples_.push_back(sample);
  }
  return result;
}

std::vector<Vec3> SculptSurfaceBridge::compose_positions(
    const std::vector<Particle> &current_proxy_particles,
    const std::vector<Vec3> &standard_brush_positions,
    BrushPhysicsSettings settings) const
{
  if (standard_brush_positions.size() != source_positions_.size()) {
    throw std::invalid_argument("Sculpt output size mismatch");
  }
  if (!std::isfinite(settings.influence) || settings.influence < 0.0 ||
      settings.influence > 1.0) {
    throw std::invalid_argument("Invalid physics assist influence");
  }
  if (settings.mode == BrushPhysicsMode::Standard || settings.influence == 0.0) {
    return standard_brush_positions; // Exact standard-geometry bypass.
  }
  std::vector<Vec3> result;
  result.reserve(samples_.size());
  for (std::size_t i = 0; i < samples_.size(); ++i) {
    const Sample &sample = samples_[i];
    Vec3 physics_position = sample.detail_residual;
    for (int k = 0; k < 3; ++k) {
      if (sample.weights[k] == 0.0) continue;
      if (sample.particle_ids[k] >= current_proxy_particles.size()) {
        throw std::out_of_range("Invalid proxy particle index");
      }
      physics_position += current_proxy_particles[sample.particle_ids[k]].position *
                          sample.weights[k];
    }
    if (settings.mode == BrushPhysicsMode::FullPhysics) {
      result.push_back(physics_position);
    }
    else {
      const double t = settings.influence;
      result.push_back(standard_brush_positions[i] * (1.0 - t) + physics_position * t);
    }
  }
  return result;
}

}  // namespace sculpt_physics
