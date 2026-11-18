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
  result.source_positions_ = vertex_positions;
  result.samples_.reserve(vertex_positions.size());
  for (std::size_t i = 0; i < vertex_positions.size(); ++i) {
    if (!finite(vertex_positions[i]) || !finite(proxy_particles[i].position)) {
      throw std::invalid_argument("Invalid regular mesh position");
    }
    const Vec3 diff = vertex_positions[i] - proxy_particles[i].position;
    const double scale = std::max({1.0, std::abs(vertex_positions[i].x),
                                   std::abs(vertex_positions[i].y),
                                   std::abs(vertex_positions[i].z)});
    if (std::sqrt(dot(diff, diff)) > 1e-6 * scale) {
      throw std::invalid_argument("Regular Sculpt mesh and proxy coordinates must match");
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
    const Frame frame = make_frame(rest_proxy_particles[anchor.vertices[0]].position,
                                   rest_proxy_particles[anchor.vertices[1]].position,
                                   rest_proxy_particles[anchor.vertices[2]].position);
    sample.frame_u = frame.u;
    sample.frame_v = frame.v;
    sample.frame_n = frame.n;
    sample.frame_valid = frame.valid;
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
    Vec3 residual = sample.detail_residual;
    if (kind_ == SculptSurfaceKind::MultiresSamples && sample.frame_valid) {
      const Frame current = make_frame(current_proxy_particles.at(sample.particle_ids[0]).position,
                                       current_proxy_particles.at(sample.particle_ids[1]).position,
                                       current_proxy_particles.at(sample.particle_ids[2]).position);
      if (current.valid) {
        residual = current.u * dot(sample.frame_u, residual) +
                   current.v * dot(sample.frame_v, residual) +
                   current.n * dot(sample.frame_n, residual);
      }
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
