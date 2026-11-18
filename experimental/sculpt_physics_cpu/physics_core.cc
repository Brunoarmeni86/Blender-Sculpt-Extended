/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace sculpt_physics {
namespace {
constexpr double eps = 1e-12;
double length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
bool valid(double n) { return std::isfinite(n); }
bool valid(Vec3 v) { return valid(v.x) && valid(v.y) && valid(v.z); }
}  // namespace

Vec3 Vec3::operator+(const Vec3 &v) const { return {x + v.x, y + v.y, z + v.z}; }
Vec3 Vec3::operator-(const Vec3 &v) const { return {x - v.x, y - v.y, z - v.z}; }
Vec3 Vec3::operator*(double a) const { return {x * a, y * a, z * a}; }
Vec3 Vec3::operator/(double a) const { return {x / a, y / a, z / a}; }
Vec3 &Vec3::operator+=(const Vec3 &v)
{
  x += v.x; y += v.y; z += v.z; return *this;
}

SurfaceSample evaluate_anchor(const SurfaceAnchor &anchor,
                              const std::vector<Particle> &particles)
{
  SurfaceSample out{};
  const double sum = anchor.weights[0] + anchor.weights[1] + anchor.weights[2];
  if (!valid(sum) || std::abs(sum - 1.0) > 1e-6) {
    throw std::invalid_argument("Anchor barycentric weights must sum to one");
  }
  for (int k = 0; k != 3; ++k) {
    if (anchor.vertices[k] >= particles.size() || !valid(anchor.weights[k])) {
      throw std::out_of_range("Invalid surface anchor");
    }
    const double w = anchor.weights[k];
    out.position += particles[anchor.vertices[k]].position * w;
    out.uv.u += anchor.corner_uv[k].u * w;
    out.uv.v += anchor.corner_uv[k].v * w;
  }
  return out;
}

std::size_t World::add_particle(Vec3 position, double inverse_mass, double radius)
{
  if (active_stroke_) throw std::logic_error("Cannot alter topology during stroke");
  if (!valid(position) || !valid(inverse_mass) || inverse_mass < 0.0 ||
      !valid(radius) || radius <= 0.0) {
    throw std::invalid_argument("Invalid particle parameters");
  }
  particles_.push_back({position, {}, inverse_mass, radius});
  return particles_.size() - 1;
}

std::size_t World::add_bond(std::size_t a, std::size_t b, double compliance,
                            double yield_strain, double plastic_creep,
                            double fracture_strain)
{
  if (active_stroke_) throw std::logic_error("Cannot alter bonds during stroke");
  if (a >= particles_.size() || b >= particles_.size() || a == b ||
      !valid(compliance) || compliance < 0.0 ||
      std::isnan(yield_strain) || yield_strain < 0.0 ||
      !valid(plastic_creep) || plastic_creep < 0.0 ||
      std::isnan(fracture_strain) || fracture_strain < 0.0) {
    throw std::invalid_argument("Invalid bond parameters");
  }
  const double rest = length(particles_[a].position - particles_[b].position);
  if (rest < eps) throw std::invalid_argument("Zero-length bond");
  bonds_.push_back({a, b, rest, compliance, yield_strain,
                    plastic_creep, fracture_strain, 0.0, false});
  return bonds_.size() - 1;
}

void World::set_gravity(Vec3 g)
{
  if (!valid(g)) throw std::invalid_argument("Invalid gravity");
  gravity_ = g;
}
void World::set_velocity_damping(double d)
{
  if (!valid(d) || d < 0.0) throw std::invalid_argument("Invalid damping");
  damping_ = d;
}
void World::set_ground_height(double y)
{
  if (!valid(y)) throw std::invalid_argument("Invalid ground");
  ground_y_ = y;
  ground_enabled_ = true;
}
void World::disable_ground() { ground_enabled_ = false; }

void World::begin_stroke()
{
  if (active_stroke_) throw std::logic_error("Stroke already active");
  saved_particles_ = particles_;
  saved_bonds_ = bonds_;
  targets_.clear();
  dirty_.clear();
  active_stroke_ = true;
}

bool World::apply_grab(Vec3 center, double radius, Vec3 displacement,
                       BrushPhysicsSettings settings)
{
  if (!active_stroke_) throw std::logic_error("begin_stroke required");
  if (settings.mode == BrushPhysicsMode::Standard || settings.influence == 0.0) {
    // Turn off the previous dab target as well. The Sculpt adapter must also skip
    // begin_stroke/step entirely when physics is not requested.
    targets_.clear();
    return false;
  }
  if (!valid(center) || !valid(displacement) || !valid(radius) || radius <= 0 ||
      !valid(settings.influence) || settings.influence < 0 || settings.influence > 1 ||
      !valid(settings.compliance) || settings.compliance < 0) {
    throw std::invalid_argument("Invalid brush parameters");
  }
  targets_.clear();
  const double radius_sq = radius * radius;
  for (std::size_t i = 0; i < particles_.size(); ++i) {
    if (particles_[i].inverse_mass == 0) continue;
    Vec3 offset = particles_[i].position - center;
    const double dist_sq = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
    if (dist_sq >= radius_sq) continue;
    const double q = 1.0 - std::sqrt(dist_sq) / radius;
    const double w = q * q * (3.0 - 2.0 * q) * settings.influence;
    if (w <= eps) continue;
    targets_.push_back({i, particles_[i].position + displacement * w,
                        settings.compliance / w, 0.0, 0.0, 0.0});
  }
  return !targets_.empty();
}

void World::step(double dt, int substeps, int iterations)
{
  if (!active_stroke_) throw std::logic_error("begin_stroke required");
  if (!valid(dt) || dt <= 0.0 || substeps <= 0 || iterations <= 0 ||
      substeps > 256 || iterations > 256) {
    throw std::invalid_argument("Invalid time step or iteration counts");
  }
  const double h = dt / substeps;
  const double attenuation = std::exp(-damping_ * h);
  // Dirty regions are consumed between dabs for PBVH updates, so compare against
  // the start of THIS step, not against the beginning of the entire stroke.
  std::vector<Vec3> step_start_positions;
  step_start_positions.reserve(particles_.size());
  for (const Particle &p : particles_) step_start_positions.push_back(p.position);
  std::vector<Vec3> old_positions(particles_.size());
  for (int s = 0; s < substeps; ++s) {
    for (std::size_t i = 0; i < particles_.size(); ++i) {
      Particle &p = particles_[i];
      old_positions[i] = p.position;
      if (p.inverse_mass <= 0) continue;
      p.velocity += gravity_ * h;
      p.position += p.velocity * h;
    }
    for (Bond &b : bonds_) b.lambda = 0.0;
    for (GrabTarget &t : targets_) t.lambda_x = t.lambda_y = t.lambda_z = 0.0;
    for (int iter = 0; iter < iterations; ++iter) {
      // Apply the moving brush target before material bonds, so nearby particles
      // can respond in the first iteration even with only one substep.
      for (GrabTarget &t : targets_) {
        Particle &p = particles_[t.index];
        const double alpha = t.compliance / (h * h);
        const double denom = p.inverse_mass + alpha;
        if (denom <= eps) continue;
        const Vec3 C = p.position - t.goal;
        const Vec3 lambda{t.lambda_x, t.lambda_y, t.lambda_z};
        const Vec3 dlambda = (C * -1.0 - lambda * alpha) / denom;
        p.position += dlambda * p.inverse_mass;
        t.lambda_x += dlambda.x; t.lambda_y += dlambda.y; t.lambda_z += dlambda.z;
      }
      for (Bond &b : bonds_) {
        if (b.broken) continue;
        Particle &a = particles_[b.a];
        Particle &c = particles_[b.b];
        Vec3 direction = c.position - a.position;
        const double len = length(direction);
        if (len < eps) continue;
        const double strain = (len - b.rest_length) / b.rest_length;
        if (std::abs(strain) > b.fracture_strain) {
          b.broken = true;
          continue;
        }
        /* Plastic creep occurs once per substep, not once per solver iteration. */
        if (iter == 0 && std::abs(strain) > b.yield_strain && b.plastic_creep > 0) {
          const double rate = 1.0 - std::exp(-b.plastic_creep * h);
          b.rest_length = std::max(eps, b.rest_length + (len - b.rest_length) * rate);
        }
        const double C = len - b.rest_length;
        const double alpha = b.compliance / (h * h);
        const double denom = a.inverse_mass + c.inverse_mass + alpha;
        if (denom <= eps) continue;
        const double dlambda = (-C - alpha * b.lambda) / denom;
        b.lambda += dlambda;
        direction = direction / len;
        a.position += direction * (-a.inverse_mass * dlambda);
        c.position += direction * (c.inverse_mass * dlambda);
      }
      if (ground_enabled_) {
        for (Particle &p : particles_) {
          if (p.inverse_mass > 0.0) {
            p.position.y = std::max(p.position.y, ground_y_ + p.radius);
          }
        }
      }
    }
    for (std::size_t i = 0; i < particles_.size(); ++i) {
      Particle &p = particles_[i];
      if (p.inverse_mass <= 0) continue;
      p.velocity = (p.position - old_positions[i]) / h * attenuation;
      if (ground_enabled_ && p.position.y <= ground_y_ + p.radius + eps && p.velocity.y < 0) {
        p.velocity.y = 0.0;
      }
      if (!valid(p.position) || !valid(p.velocity)) {
        throw std::runtime_error("Physics state became non-finite");
      }
    }
  }
  dirty_.clear();
  for (std::size_t i = 0; i < particles_.size(); ++i) {
    const Vec3 delta = particles_[i].position - step_start_positions[i];
    if (length(delta) > 1e-10) dirty_.push_back(i);
  }
}

void World::commit_stroke()
{
  if (!active_stroke_) throw std::logic_error("No active stroke");
  saved_particles_.clear();
  saved_bonds_.clear();
  targets_.clear();
  active_stroke_ = false;
}
void World::cancel_stroke()
{
  if (!active_stroke_) throw std::logic_error("No active stroke");
  particles_ = std::move(saved_particles_);
  bonds_ = std::move(saved_bonds_);
  targets_.clear();
  dirty_.clear();
  active_stroke_ = false;
}
bool World::stroke_active() const { return active_stroke_; }
const std::vector<Particle> &World::particles() const { return particles_; }
const std::vector<Bond> &World::bonds() const { return bonds_; }
const std::vector<std::size_t> &World::dirty_particles() const { return dirty_; }

}  // namespace sculpt_physics
