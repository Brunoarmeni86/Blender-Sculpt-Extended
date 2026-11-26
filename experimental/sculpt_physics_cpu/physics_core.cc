/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace sculpt_physics {
namespace {
constexpr double eps = 1e-12;
double length(Vec3 v)
{
  // Fast path at ordinary object scales, robust fallback at extreme scales.
  const double square = v.x * v.x + v.y * v.y + v.z * v.z;
  if (std::isfinite(square) && square > 0.0) return std::sqrt(square);
  if (v.x == 0.0 && v.y == 0.0 && v.z == 0.0) return 0.0;
  return std::hypot(v.x, v.y, v.z);
}
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

void World::reserve_topology(std::size_t particles, std::size_t bonds)
{
  if (active_stroke_) throw std::logic_error("Cannot reserve topology during stroke");
  particles_.reserve(particles);
  particle_bond_ids_.reserve(particles);
  bonds_.reserve(bonds);
}

std::size_t World::add_particle(Vec3 position, double inverse_mass, double radius)
{
  if (active_stroke_) throw std::logic_error("Cannot alter topology during stroke");
  if (!valid(position) || !valid(inverse_mass) || inverse_mass < 0.0 ||
      !valid(radius) || radius <= 0.0) {
    throw std::invalid_argument("Invalid particle parameters");
  }
  // Maintain exact particle/adjoining-graph parity on allocation failure.
  // Otherwise a failed topology operation can corrupt later Undo/BFS indexing.
  particle_bond_ids_.emplace_back();
  try {
    particles_.push_back({position, {}, inverse_mass, radius});
  }
  catch (...) {
    particle_bond_ids_.pop_back();
    throw;
  }
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
  const Vec3 rest_delta = particles_[a].position - particles_[b].position;
  const double rest = std::hypot(rest_delta.x, rest_delta.y, rest_delta.z);
  if (!valid(rest) || rest < eps) throw std::invalid_argument("Invalid bond length");
  // All-or-nothing graph update. A failed adjacency allocation must never
  // leave a bond that references only one endpoint.
  const std::size_t id = bonds_.size();
  bonds_.push_back({a, b, rest, compliance, yield_strain,
                    plastic_creep, fracture_strain, 0.0, false});
  try {
    particle_bond_ids_[a].push_back(id);
  }
  catch (...) {
    bonds_.pop_back();
    throw;
  }
  try {
    particle_bond_ids_[b].push_back(id);
  }
  catch (...) {
    particle_bond_ids_[a].pop_back();
    bonds_.pop_back();
    throw;
  }
  return id;
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

World::StrokeJournalStatistics World::stroke_journal_statistics() const
{
  if (dense_journal_) {
    return {dense_undo_particles_.size(), dense_undo_bonds_.size(),
            dense_undo_particles_.size() * sizeof(Particle) +
                dense_undo_bonds_.size() * sizeof(Bond), true};
  }
  return {undo_particles_.size(), undo_bonds_.size(),
          undo_particles_.size() * sizeof(Particle) + undo_bonds_.size() * sizeof(Bond), false};
}

void World::begin_stroke()
{
  if (active_stroke_) throw std::logic_error("Stroke already active");
  undo_particles_.clear();
  undo_bonds_.clear();
  dense_undo_particles_.clear();
  dense_undo_bonds_.clear();
  dense_journal_ = false;
  saved_moving_particle_ids_ = moving_particle_ids_;
  saved_ground_enabled_ = ground_enabled_;
  saved_ground_y_ = ground_y_;
  saved_damping_ = damping_;
  saved_gravity_ = gravity_;
  targets_.clear();
  pending_targets_.clear();
  dirty_.clear();
  active_region_enabled_ = false;
  adaptive_cached_hops_ = -1;
  active_stroke_ = true;
}

bool World::apply_grab(Vec3 center, double radius, Vec3 displacement,
                       BrushPhysicsSettings settings)
{
  return apply_grab_impl(center, radius, displacement, settings, nullptr);
}

bool World::apply_grab_candidates(Vec3 center, double radius, Vec3 displacement,
                                  BrushPhysicsSettings settings,
                                  const std::vector<std::size_t> &candidate_ids)
{
  if (!active_stroke_) throw std::logic_error("begin_stroke required");
  if (settings.mode == BrushPhysicsMode::Standard || settings.influence == 0.0) {
    targets_.clear();
    return false;
  }
  // Stable order is required for reproducible Gauss-Seidel brush constraints.
  // This is O(K log K), but K is the PBVH-selected region, not all N particles.
  brush_candidate_scratch_ = candidate_ids;
  std::sort(brush_candidate_scratch_.begin(), brush_candidate_scratch_.end());
  brush_candidate_scratch_.erase(
      std::unique(brush_candidate_scratch_.begin(), brush_candidate_scratch_.end()),
      brush_candidate_scratch_.end());
  return apply_grab_impl(center, radius, displacement, settings, &brush_candidate_scratch_);
}

bool World::set_brush_targets(const std::vector<BrushTargetInput> &inputs)
{
  if (!active_stroke_) throw std::logic_error("begin_stroke required");
  // Both candidate duplication and invalid values are rejected before
  // replacing an accepted target list; the pending vector is reused.
  pending_targets_.clear();
  pending_targets_.reserve(inputs.size());
  for (const BrushTargetInput &in : inputs) {
    if (in.particle_index >= particles_.size() ||
        !valid(in.goal) || !valid(in.compliance) || in.compliance < 0.0) {
      throw std::invalid_argument("Invalid native Sculpt brush constraint");
    }
    if (particles_[in.particle_index].inverse_mass <= 0.0) continue;
    pending_targets_.push_back({in.particle_index, in.goal, in.compliance, 0.0, 0.0, 0.0});
  }
  std::sort(pending_targets_.begin(), pending_targets_.end(),
            [](const GrabTarget &a, const GrabTarget &b) { return a.index < b.index; });
  for (std::size_t i = 1; i < pending_targets_.size(); ++i) {
    if (pending_targets_[i].index == pending_targets_[i-1].index) {
      throw std::invalid_argument("Duplicate native Sculpt brush target");
    }
  }
  targets_.swap(pending_targets_);
  return !targets_.empty();
}

bool World::set_active_region(const std::vector<std::size_t> &particle_ids)
{
  if (!active_stroke_) throw std::logic_error("begin_stroke required");
  if (active_region_marks_.size() != particles_.size()) {
    active_region_marks_.assign(particles_.size(), 0);
  }
  if (++active_region_generation_ == 0) {
    std::fill(active_region_marks_.begin(), active_region_marks_.end(), 0);
    active_region_generation_ = 1;
  }
  for (const std::size_t particle : particle_ids) {
    if (particle >= particles_.size()) {
      active_region_enabled_ = false;
      return false;
    }
    active_region_marks_[particle] = active_region_generation_;
  }
  active_region_enabled_ = !particle_ids.empty();
  return true;
}

bool World::apply_grab_impl(Vec3 center, double radius, Vec3 displacement,
                            BrushPhysicsSettings settings,
                            const std::vector<std::size_t> *candidate_ids)
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
  // Construct targets transactionally, keeping the last valid dab on error.
  // Scratch capacity is reused to avoid heap churn between brush updates.
  pending_targets_.clear();
  const double radius_sq = radius * radius;
  auto visit = [&](std::size_t i) {
    if (i >= particles_.size()) throw std::out_of_range("Invalid PBVH Grab candidate index");
    if (particles_[i].inverse_mass == 0) return;
    const Vec3 offset = particles_[i].position - center;
    const double dist_sq = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
    if (std::isfinite(radius_sq) && radius_sq > 0.0 && dist_sq >= radius_sq) return;
    const double distance = (std::isfinite(radius_sq) && radius_sq > 0.0) ? std::sqrt(dist_sq) :
                            std::hypot(offset.x, offset.y, offset.z);
    if (!(distance < radius)) return;
    const double q = 1.0 - distance / radius;
    // Assist strength is applied only in the surface bridge. FullPhysics
    // still scales its physical target here, avoiding quadratic attenuation.
    const double target_strength = settings.mode == BrushPhysicsMode::PhysicalAssist ?
                                       1.0 : settings.influence;
    const double w = q * q * (3.0 - 2.0 * q) * target_strength;
    if (w <= eps) return;
    const Vec3 goal = particles_[i].position + displacement * w;
    const double effective_compliance = settings.compliance / w;
    if (!valid(goal) || !valid(effective_compliance)) {
      throw std::invalid_argument("Grab produced non-finite target parameters");
    }
    pending_targets_.push_back({i, goal, effective_compliance, 0.0, 0.0, 0.0});
  };
  if (candidate_ids) {
    for (std::size_t i : *candidate_ids) visit(i);
  }
  else {
    for (std::size_t i = 0; i < particles_.size(); ++i) visit(i);
  }
  targets_.swap(pending_targets_);
  return !targets_.empty();
}

void World::step(double dt, int substeps, int iterations)
{
  if (!active_stroke_) throw std::logic_error("begin_stroke required");
  if (!valid(dt) || dt <= 0.0 || substeps <= 0 || iterations <= 0 ||
      substeps > 256 || iterations > 256) {
    throw std::invalid_argument("Invalid time step or iteration counts");
  }
  // XPBD compliance/(h*h) is undefined when the squared substep underflows.
  const double probe_h = dt / substeps;
  if (probe_h * probe_h < std::numeric_limits<double>::min()) {
    throw std::invalid_argument("Substep too small for XPBD constraint denominator");
  }
  // Preserve the last accepted dab if a numerical step fails.
  // A global step needs an O(N) current-state checkpoint.
  auto step_particles = particles_;
  auto step_bonds = bonds_;
  auto step_targets = targets_;
  auto step_dirty = dirty_;
  auto step_movers = moving_particle_ids_;
  const StepStatistics step_stats = last_stats_;
  try {
    const double h = dt / substeps;
    const double attenuation = std::exp(-damping_ * h);
    // Convert the current-step checkpoint into dense Undo only after success.
    // A failed dab must preserve the existing sparse stroke journal.
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
          if (!valid(alpha)) throw std::runtime_error("Grab XPBD compliance out of range");
          const double denom = p.inverse_mass + alpha;
          if (!valid(denom)) throw std::runtime_error("XPBD effective mass out of range");
          if (denom <= 0.0) continue;
          const Vec3 C = p.position - t.goal;
          const Vec3 lambda{t.lambda_x, t.lambda_y, t.lambda_z};
          const Vec3 dlambda = (C * -1.0 - lambda * alpha) / denom;
          if (!valid(dlambda)) throw std::runtime_error("Grab XPBD multiplier out of range");
          p.position += dlambda * p.inverse_mass;
          t.lambda_x += dlambda.x; t.lambda_y += dlambda.y; t.lambda_z += dlambda.z;
        }
        for (Bond &b : bonds_) {
          if (b.broken) continue;
          Particle &a = particles_[b.a];
          Particle &c = particles_[b.b];
          Vec3 direction = c.position - a.position;
          const double len = length(direction);
          if (!valid(len)) throw std::runtime_error("Bond length out of range");
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
          if (!valid(alpha)) throw std::runtime_error("Bond XPBD compliance out of range");
          const double denom = a.inverse_mass + c.inverse_mass + alpha;
          if (!valid(denom)) throw std::runtime_error("XPBD effective mass out of range");
          if (denom <= 0.0) continue;
          const double dlambda = (-C - alpha * b.lambda) / denom;
          if (!valid(dlambda) || !valid(b.lambda + dlambda)) {
            throw std::runtime_error("Bond XPBD multiplier out of range");
          }
          b.lambda += dlambda;
          direction = direction / len;
          if (a.inverse_mass > 0.0) a.position += direction * (-a.inverse_mass * dlambda);
          if (c.inverse_mass > 0.0) c.position += direction * (c.inverse_mass * dlambda);
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
        if (p.inverse_mass > 0.0) {
          p.velocity = (p.position - old_positions[i]) / h * attenuation;
          if (ground_enabled_ && p.position.y <= ground_y_ + p.radius + eps && p.velocity.y < 0) {
            p.velocity.y = 0.0;
          }
        }
        // Fixed bodies must also remain valid; NaNs are not acceptable.
        if (!valid(p.position) || !valid(p.velocity)) {
          throw std::runtime_error("Physics state became non-finite");
        }
      }
    }
    dirty_.clear();
    moving_particle_ids_.clear();
    last_stats_ = {particles_.size(), bonds_.size() * std::size_t(substeps) * std::size_t(iterations), false};
    for (std::size_t i = 0; i < particles_.size(); ++i) {
      const Vec3 delta = particles_[i].position - step_start_positions[i];
      if (length(delta) > 1e-10) dirty_.push_back(i);
      const Vec3 v = particles_[i].velocity;
      if (v.x != 0.0 || v.y != 0.0 || v.z != 0.0) moving_particle_ids_.push_back(i);
    }
    if (!dense_journal_) {
      // The rollback snapshot is the pre-global-dab state. Overlay pre-STROKE
      // sparse originals from earlier local dabs, without a second full copy.
      for (const auto &entry : undo_particles_) step_particles[entry.first] = entry.second;
      for (const auto &entry : undo_bonds_) step_bonds[entry.first] = entry.second;
      dense_undo_particles_ = std::move(step_particles);
      dense_undo_bonds_ = std::move(step_bonds);
      dense_journal_ = true;
      undo_particles_.clear();
      undo_bonds_.clear();
    }
  }
  catch (...) {
    particles_.swap(step_particles);
    bonds_.swap(step_bonds);
    targets_.swap(step_targets);
    dirty_.swap(step_dirty);
    moving_particle_ids_.swap(step_movers);
    last_stats_ = step_stats;
    throw;
  }
}

void World::step_local(double dt, int substeps, int iterations, int bond_hops)
{
  step_local_impl(dt, substeps, iterations, bond_hops, false);
}

void World::step_local_multi(double dt, int substeps, int iterations, int bond_hops)
{
  step_local_impl(dt, substeps, iterations, bond_hops, true);
}

void World::step_local_impl(double dt, int substeps, int iterations, int bond_hops,
                            bool include_moving_islands)
{
  if (!active_stroke_) throw std::logic_error("begin_stroke required");
  if (!valid(dt) || dt <= 0.0 || substeps <= 0 || iterations <= 0 ||
      substeps > 256 || iterations > 256 || bond_hops < 0 || bond_hops > 256) {
    throw std::invalid_argument("Invalid local physics parameters");
  }
  // XPBD compliance/(h*h) is undefined when the squared substep underflows.
  const double probe_h = dt / substeps;
  if (probe_h * probe_h < std::numeric_limits<double>::min()) {
    throw std::invalid_argument("Substep too small for XPBD constraint denominator");
  }
  // Without an active target, a quasistatic sculpt dab must be a genuine no-op.
  // Do not wake or advance the entire object just because the brush missed it.
  // Scene simulation with gravity/contact still requires global integration.
  if (targets_.empty() && !ground_enabled_ && length(gravity_) <= eps) {
    dirty_.clear();
    last_stats_ = {};
    return;
  }
  // A local brush island is an approximation for quasistatic strokes only.
  // Global gravity/contact requires global integration.
  if (ground_enabled_ || length(gravity_) > eps) {
    step(dt, substeps, iterations);
    last_stats_.used_global_fallback = true;
    return;
  }
  const double h = dt / substeps;
  const double attenuation = std::exp(-damping_ * h);
  // Generation marks avoid clearing an N-particle visited array each dab.
  if (included_generation_marks_.size() != particles_.size()) {
    included_generation_marks_.assign(particles_.size(), 0);
  }
  if (++included_generation_ == 0) {
    std::fill(included_generation_marks_.begin(), included_generation_marks_.end(), 0);
    included_generation_ = 1;
  }
  const auto included = [&](std::size_t i) {
    return included_generation_marks_[i] == included_generation_;
  };
  const auto include = [&](std::size_t i) {
    included_generation_marks_[i] = included_generation_;
  };
  const auto allowed = [&](const std::size_t i) {
    return !active_region_enabled_ || active_region_marks_[i] == active_region_generation_;
  };
  std::vector<std::size_t> &active = local_active_scratch_;
  active.clear();
  for (const GrabTarget &target : targets_) {
    if (!allowed(target.index)) {
      throw std::invalid_argument("Brush target lies outside active Undo region");
    }
    if (!included(target.index)) {
      include(target.index);
      active.push_back(target.index);
    }
  }
  // Seed any separately moving islands as well as the active brush targets.
  // Unlike the legacy local path, multi-island mode advances these components
  // without forcing an unrelated global step.
  if (include_moving_islands) {
    const double minimum_speed = 1.0e-10 / dt;
    for (const std::size_t pi : moving_particle_ids_) {
      if (pi >= particles_.size()) throw std::logic_error("Stale moving-particle index");
      const Particle &p = particles_[pi];
      if (p.inverse_mass <= 0.0 || included(pi) || !allowed(pi)) continue;
      const Vec3 v = p.velocity;
      const double component_speed = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
      if (!std::isfinite(component_speed)) {
        throw std::runtime_error("Non-finite velocity in moving island");
      }
      if (component_speed > minimum_speed) {
        include(pi);
        active.push_back(pi);
      }
    }
  }
  // Expand through material bonds. Stable ordering is important for repeatable
  // constraint solutions; avoid unordered iteration of the solver itself.
  std::size_t front_begin = 0;
  std::size_t front_end = active.size();
  for (int hop = 0; hop < bond_hops; ++hop) {
    for (std::size_t i = front_begin; i < front_end; ++i) {
      const std::size_t pi = active[i];
      for (const std::size_t bi : particle_bond_ids_[pi]) {
        const Bond &b = bonds_[bi];
        if (b.broken) continue;
        const std::size_t other = b.a == pi ? b.b : b.a;
        if (!included(other) && allowed(other)) {
          include(other);
          active.push_back(other);
        }
      }
    }
    front_begin = front_end;
    front_end = active.size();
    if (front_begin == front_end) break;
  }
  // A brush-centered island must not freeze a different region that is still
  // moving from an earlier dab/stroke. Check predicted motion for this time step.
  // This O(N) check is a correctness-first baseline; a sparse motion index can
  // make it sublinear later.
  const double minimum_speed = 1.0e-10 / dt;
  bool exterior_motion = false;
  if (!include_moving_islands && !active_region_enabled_) {
    for (const std::size_t i : moving_particle_ids_) {
      if (included(i) || particles_[i].inverse_mass <= 0.0) continue;
    const Vec3 v = particles_[i].velocity;
    const double component_speed = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
      if (!std::isfinite(component_speed) || component_speed > minimum_speed) {
        exterior_motion = true;
        break;
      }
    }
  }
  if (exterior_motion) {
    // World::step already provides exact rollback for a failed global dab.
    step(dt, substeps, iterations);
    last_stats_.used_global_fallback = true;
    return;
  }
  // Bonds touching the active island are solved in their original index order.
  // The halo boundary is pinned; only active particles receive changes.
  std::vector<std::size_t> &selected_bonds = local_bond_scratch_;
  selected_bonds.clear();
  for (const std::size_t i : active) {
    selected_bonds.insert(selected_bonds.end(),
                          particle_bond_ids_[i].begin(), particle_bond_ids_[i].end());
  }
  std::sort(selected_bonds.begin(), selected_bonds.end());
  selected_bonds.erase(std::unique(selected_bonds.begin(), selected_bonds.end()),
                       selected_bonds.end());
  // A rejected dab is a transaction: its first-touch Undo entries must
  // disappear together with its failed geometric changes.
  local_undo_new_particle_ids_.clear();
  local_undo_new_bond_ids_.clear();
  if (!dense_journal_) {
    // Ensure adding ID keys during rollback tracking cannot allocate after a
    // first-touch hash insertion, preserving exception safety.
    local_undo_new_particle_ids_.reserve(active.size());
    local_undo_new_bond_ids_.reserve(selected_bonds.size());
  }
  std::vector<Vec3> &step_start = local_start_scratch_;
  std::vector<Vec3> &previous = local_previous_scratch_;
  step_start.clear();
  step_start.reserve(active.size());
  previous.resize(active.size());
  for (const std::size_t i : active) step_start.push_back(particles_[i].position);
  // Protect only the active island and touching bonds. The sparse stroke
  // journal is for whole-stroke Undo; this checkpoint is for one failed dab.
  std::vector<Particle> step_particles;
  step_particles.reserve(active.size());
  for (const std::size_t i : active) step_particles.push_back(particles_[i]);
  std::vector<Bond> step_bonds;
  step_bonds.reserve(selected_bonds.size());
  for (const std::size_t bi : selected_bonds) step_bonds.push_back(bonds_[bi]);
  auto step_targets = targets_;
  auto step_dirty = dirty_;
  auto step_movers = moving_particle_ids_;
  const StepStatistics step_stats = last_stats_;
  try {
    if (!dense_journal_) {
      for (const std::size_t i : active) {
        if (particles_[i].inverse_mass > 0.0 &&
            undo_particles_.try_emplace(i, particles_[i]).second) {
          local_undo_new_particle_ids_.push_back(i);
        }
      }
      for (const std::size_t bi : selected_bonds) {
        if (undo_bonds_.try_emplace(bi, bonds_[bi]).second) {
          local_undo_new_bond_ids_.push_back(bi);
        }
      }
    }
    for (int sub = 0; sub < substeps; ++sub) {
      for (std::size_t j = 0; j < active.size(); ++j) {
        Particle &p = particles_[active[j]];
        previous[j] = p.position;
        if (p.inverse_mass > 0.0) p.position += p.velocity * h;
      }
      for (const std::size_t bi : selected_bonds) bonds_[bi].lambda = 0.0;
      for (GrabTarget &t : targets_) t.lambda_x = t.lambda_y = t.lambda_z = 0.0;
      for (int iter = 0; iter < iterations; ++iter) {
        for (GrabTarget &t : targets_) {
          Particle &p = particles_[t.index];
          const double alpha = t.compliance / (h * h);
          if (!valid(alpha)) throw std::runtime_error("Grab XPBD compliance out of range");
          const double denom = p.inverse_mass + alpha;
          if (!valid(denom)) throw std::runtime_error("XPBD effective mass out of range");
          if (denom <= 0.0) continue;
          const Vec3 C = p.position - t.goal;
          const Vec3 lambda{t.lambda_x, t.lambda_y, t.lambda_z};
          const Vec3 dlambda = (C * -1.0 - lambda * alpha) / denom;
          if (!valid(dlambda)) throw std::runtime_error("Grab XPBD multiplier out of range");
          p.position += dlambda * p.inverse_mass;
          t.lambda_x += dlambda.x; t.lambda_y += dlambda.y; t.lambda_z += dlambda.z;
        }
        for (const std::size_t bi : selected_bonds) {
          Bond &b = bonds_[bi];
          if (b.broken) continue;
          Particle &a = particles_[b.a];
          Particle &c = particles_[b.b];
          Vec3 direction = c.position - a.position;
          const double len = length(direction);
          if (!valid(len)) throw std::runtime_error("Bond length out of range");
          if (len < eps) continue;
          const double strain = (len - b.rest_length) / b.rest_length;
          // The local halo boundary is a numerical pin, not a physical pin:
          // it must not produce irreversible fracture or plastic rest changes.
          const bool fully_active_bond = included(b.a) && included(b.b);
          if (fully_active_bond && std::abs(strain) > b.fracture_strain) {
            b.broken = true;
            continue;
          }
          if (fully_active_bond && iter == 0 && std::abs(strain) > b.yield_strain &&
              b.plastic_creep > 0) {
            const double rate = 1.0 - std::exp(-b.plastic_creep * h);
            b.rest_length = std::max(eps, b.rest_length + (len - b.rest_length) * rate);
          }
          const double C = len - b.rest_length;
          const double alpha = b.compliance / (h * h);
          if (!valid(alpha)) throw std::runtime_error("Bond XPBD compliance out of range");
          const double inv_a = included(b.a) ? a.inverse_mass : 0.0;
          const double inv_c = included(b.b) ? c.inverse_mass : 0.0;
          const double denom = inv_a + inv_c + alpha;
          if (!valid(denom)) throw std::runtime_error("XPBD effective mass out of range");
          if (denom <= 0.0) continue;
          const double dlambda = (-C - alpha * b.lambda) / denom;
          if (!valid(dlambda) || !valid(b.lambda + dlambda)) {
            throw std::runtime_error("Bond XPBD multiplier out of range");
          }
          b.lambda += dlambda;
          direction = direction / len;
          if (inv_a > 0.0) a.position += direction * (-inv_a * dlambda);
          if (inv_c > 0.0) c.position += direction * (inv_c * dlambda);
        }
      }
      for (std::size_t j = 0; j < active.size(); ++j) {
        Particle &p = particles_[active[j]];
        if (p.inverse_mass <= 0.0) continue;
        p.velocity = (p.position - previous[j]) / h * attenuation;
        if (!valid(p.position) || !valid(p.velocity)) {
          throw std::runtime_error("Local physics state became non-finite");
        }
      }
    }
    dirty_.clear();
    for (std::size_t j = 0; j < active.size(); ++j) {
      if (length(particles_[active[j]].position - step_start[j]) > 1e-10) {
        dirty_.push_back(active[j]);
      }
    }
    last_stats_ = {active.size(), selected_bonds.size() * std::size_t(substeps) *
                   std::size_t(iterations), false};
    // Preserve motion across strokes without an O(N) scan on every brush dab.
    moving_particle_ids_.insert(moving_particle_ids_.end(), active.begin(), active.end());
    std::sort(moving_particle_ids_.begin(), moving_particle_ids_.end());
    moving_particle_ids_.erase(
        std::unique(moving_particle_ids_.begin(), moving_particle_ids_.end()),
        moving_particle_ids_.end());
    moving_particle_ids_.erase(
        std::remove_if(moving_particle_ids_.begin(), moving_particle_ids_.end(),
                       [&](std::size_t i) {
                         const Vec3 v = particles_[i].velocity;
                         return v.x == 0.0 && v.y == 0.0 && v.z == 0.0;
                       }), moving_particle_ids_.end());
  }
  catch (...) {
    // Restore the per-dab journal membership, not only particles and bonds.
    for (const std::size_t i : local_undo_new_particle_ids_) undo_particles_.erase(i);
    for (const std::size_t i : local_undo_new_bond_ids_) undo_bonds_.erase(i);
    for (std::size_t j = 0; j < active.size(); ++j) {
      particles_[active[j]] = step_particles[j];
    }
    for (std::size_t j = 0; j < selected_bonds.size(); ++j) {
      bonds_[selected_bonds[j]] = step_bonds[j];
    }
    targets_.swap(step_targets);
    dirty_.swap(step_dirty);
    moving_particle_ids_.swap(step_movers);
    last_stats_ = step_stats;
    throw;
  }
}

World::StepStatistics World::last_step_statistics() const { return last_stats_; }

void World::commit_stroke()
{
  if (!active_stroke_) throw std::logic_error("No active stroke");
  undo_particles_.clear();
  undo_bonds_.clear();
  dense_undo_particles_.clear();
  dense_undo_bonds_.clear();
  dense_journal_ = false;
  saved_moving_particle_ids_.clear();
  targets_.clear();
  adaptive_cached_hops_ = -1;
  active_stroke_ = false;
}
void World::cancel_stroke()
{
  if (!active_stroke_) throw std::logic_error("No active stroke");
  if (dense_journal_) {
    particles_ = std::move(dense_undo_particles_);
    bonds_ = std::move(dense_undo_bonds_);
  }
  else {
    for (const auto &entry : undo_particles_) particles_[entry.first] = entry.second;
    for (const auto &entry : undo_bonds_) bonds_[entry.first] = entry.second;
  }
  undo_particles_.clear();
  undo_bonds_.clear();
  dense_journal_ = false;
  moving_particle_ids_ = std::move(saved_moving_particle_ids_);
  ground_enabled_ = saved_ground_enabled_;
  ground_y_ = saved_ground_y_;
  damping_ = saved_damping_;
  gravity_ = saved_gravity_;
  targets_.clear();
  dirty_.clear();
  adaptive_cached_hops_ = -1;
  active_stroke_ = false;
}
bool World::stroke_active() const { return active_stroke_; }
const std::vector<Particle> &World::particles() const { return particles_; }
const std::vector<Bond> &World::bonds() const { return bonds_; }
const std::vector<std::size_t> &World::dirty_particles() const { return dirty_; }

}  // namespace sculpt_physics
