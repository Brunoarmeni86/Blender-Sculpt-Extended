/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sculpt_surface_bridge.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sculpt_physics {
namespace {
bool finite(Vec3 v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
double dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b)
{
  return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
struct Frame { Vec3 u{}, v{}, n{}; bool valid = false; };
Frame make_frame(Vec3 a, Vec3 b, Vec3 c)
{
  const Vec3 e = b - a;
  const Vec3 n_raw = cross(e, c-a);
  const double e_len = std::sqrt(dot(e, e));
  const double n_len = std::sqrt(dot(n_raw, n_raw));
  // A degenerate triangle cannot define a detail frame. Avoid NaN output.
  if (!std::isfinite(e_len) || !std::isfinite(n_len) ||
      e_len < 1e-12 || n_len < 1e-12) return {};
  Frame frame;
  frame.u = e / e_len;
  frame.n = n_raw / n_len;
  frame.v = cross(frame.n, frame.u);
  frame.valid = true;
  return frame;
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
  result.sample_count_ = vertex_positions.size();
  // A large object-space translation must not inflate alignment tolerance.
  Vec3 lower{}, upper{};
  if (!vertex_positions.empty()) {
    lower = upper = vertex_positions.front();
    for (const Vec3 &v : vertex_positions) {
      if (!finite(v)) throw std::invalid_argument("Invalid regular mesh position");
      lower.x = std::min(lower.x, v.x); lower.y = std::min(lower.y, v.y);
      lower.z = std::min(lower.z, v.z);
      upper.x = std::max(upper.x, v.x); upper.y = std::max(upper.y, v.y);
      upper.z = std::max(upper.z, v.z);
    }
  }
  const Vec3 extent = upper - lower;
  const double tolerance = std::max(1e-6, std::min(1e-3,
      1e-7 * std::max({extent.x, extent.y, extent.z})));
  for (std::size_t i = 0; i < vertex_positions.size(); ++i) {
    if (!finite(vertex_positions[i]) || !finite(proxy_particles[i].position)) {
      throw std::invalid_argument("Invalid regular mesh position");
    }
    const Vec3 diff = vertex_positions[i] - proxy_particles[i].position;
    if (std::hypot(diff.x, diff.y, diff.z) > tolerance) {
      throw std::invalid_argument("Regular Sculpt mesh and proxy coordinates must match");
    }
    // No per-vertex anchor allocations: normal Sculpt already has 1:1 indices.
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
  result.sample_count_ = evaluated_positions.size();
  result.samples_.reserve(anchors.size());
  for (std::size_t i = 0; i < anchors.size(); ++i) {
    if (!finite(evaluated_positions[i])) {
      throw std::invalid_argument("Invalid evaluated Multires position");
    }
    Sample sample;
    const SurfaceAnchor &anchor = anchors[i];
    const Vec3 rest = anchored_position(anchor, rest_proxy_particles);
    if (!finite(rest)) throw std::invalid_argument("Non-finite Multires rest anchor");
    for (int k = 0; k < 3; ++k) {
      sample.particle_ids[k] = anchor.vertices[k];
      sample.weights[k] = anchor.weights[k];
    }
    const Vec3 world_residual = evaluated_positions[i] - rest;
    if (!finite(world_residual)) throw std::invalid_argument("Non-finite Multires detail residual");
    const Frame frame = make_frame(rest_proxy_particles[anchor.vertices[0]].position,
                                   rest_proxy_particles[anchor.vertices[1]].position,
                                   rest_proxy_particles[anchor.vertices[2]].position);
    sample.frame_valid = frame.valid;
    // One Vec3 of local frame components replaces three per-sample frame axes.
    sample.detail_residual = frame.valid ?
        Vec3{dot(frame.u, world_residual), dot(frame.v, world_residual),
             dot(frame.n, world_residual)} : world_residual;
    result.samples_.push_back(sample);
  }
  return result;
}

std::vector<Vec3> SculptSurfaceBridge::compose_positions(
    const std::vector<Particle> &current_proxy_particles,
    const std::vector<Vec3> &standard_brush_positions,
    BrushPhysicsSettings settings) const
{
  if (standard_brush_positions.size() != sample_count_) {
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
  result.reserve(sample_count_);
  if (kind_ == SculptSurfaceKind::RegularMesh) {
    if (current_proxy_particles.size() != sample_count_) {
      throw std::invalid_argument("Regular Sculpt proxy vertex count changed");
    }
    for (std::size_t i = 0; i < sample_count_; ++i) {
      const Vec3 physical = current_proxy_particles[i].position;
      if (!finite(physical)) throw std::invalid_argument("Invalid regular proxy position");
      if (settings.mode == BrushPhysicsMode::FullPhysics) {
        result.push_back(physical);
      }
      else {
        const Vec3 blended = standard_brush_positions[i] * (1.0-settings.influence) +
                             physical * settings.influence;
        if (!finite(blended)) {
          throw std::invalid_argument("Non-finite regular Sculpt Assist output");
        }
        result.push_back(blended);
      }
    }
    return result;
  }
  for (std::size_t i = 0; i < samples_.size(); ++i) {
    const Sample &sample = samples_[i];
    Vec3 residual = sample.detail_residual;
    if (sample.frame_valid) {
      const Frame current = make_frame(current_proxy_particles.at(sample.particle_ids[0]).position,
                                       current_proxy_particles.at(sample.particle_ids[1]).position,
                                       current_proxy_particles.at(sample.particle_ids[2]).position);
      if (!current.valid) {
        throw std::invalid_argument("Degenerate deformed Multires anchor: unsafe detail transfer");
      }
      residual = current.u * sample.detail_residual.x +
                 current.v * sample.detail_residual.y +
                 current.n * sample.detail_residual.z;
    }
    Vec3 physics_position = residual;
    for (int k = 0; k < 3; ++k) {
      if (sample.weights[k] == 0.0) continue;
      if (sample.particle_ids[k] >= current_proxy_particles.size()) {
        throw std::out_of_range("Invalid proxy particle index");
      }
      physics_position += current_proxy_particles[sample.particle_ids[k]].position *
                          sample.weights[k];
    }
    if (!finite(physics_position)) {
      throw std::invalid_argument("Non-finite reconstructed Multires sample");
    }
    if (settings.mode == BrushPhysicsMode::FullPhysics) {
      result.push_back(physics_position);
    }
    else {
      const double t = settings.influence;
      const Vec3 blended = standard_brush_positions[i] * (1.0 - t) + physics_position * t;
      if (!finite(blended)) {
        throw std::invalid_argument("Non-finite Multires assist sample");
      }
      result.push_back(blended);
    }
  }
  return result;
}

}  // namespace sculpt_physics
