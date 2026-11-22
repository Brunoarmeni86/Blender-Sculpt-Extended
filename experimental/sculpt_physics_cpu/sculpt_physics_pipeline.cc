/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sculpt_physics_pipeline.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sculpt_physics {
namespace {

bool finite(Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

/* Both brush and contact goals describe displacements relative to the SAME
 * current World position. Sum only when the same candidate has both goals;
 * this is an approximate DEM+artistic-drag coupling, not full DEM integration. */
std::vector<World::BrushTargetInput> merge_targets(
    const World &world,
    std::vector<World::BrushTargetInput> brush,
    const std::vector<World::BrushTargetInput> &contacts)
{
  std::sort(brush.begin(), brush.end(), [](const auto &a, const auto &b) {
    return a.particle_index < b.particle_index;
  });
  for (const auto &contact : contacts) {
    const auto it = std::lower_bound(
        brush.begin(), brush.end(), contact.particle_index,
        [](const auto &a, std::size_t b) { return a.particle_index < b; });
    if (it == brush.end() || it->particle_index != contact.particle_index) {
      brush.insert(it, contact);
      continue;
    }
    const Vec3 source = world.particles().at(contact.particle_index).position;
    const Vec3 mixed = it->goal + (contact.goal - source);
    if (!finite(mixed)) throw std::invalid_argument("Non-finite combined DEM/brush goal");
    it->goal = mixed;
    it->compliance = std::min(it->compliance, contact.compliance);
  }
  return brush;
}

}  // namespace

CpuStrokePipeline::~CpuStrokePipeline()
{
  if (active_) {
    try {
      cancel();
    }
    catch (...) {
      // Destructors must not throw; owner must keep World alive longer.
    }
  }
}

void CpuStrokePipeline::configure_multires(
    const std::vector<SurfaceAnchor> &anchors, std::size_t samples_per_tile)
{
  if (active_) throw std::logic_error("Cannot remap Multires during a stroke");
  if (samples_per_tile == 0) throw std::invalid_argument("Zero Multires tile size");
  auto new_map = std::make_unique<MultiresDirtyMap>(world_.particles().size(), anchors);
  multires_ = std::move(new_map);
  samples_per_tile_ = samples_per_tile;
}

void CpuStrokePipeline::clear_multires()
{
  if (active_) throw std::logic_error("Cannot unmap Multires during a stroke");
  multires_.reset();
  samples_per_tile_ = 0;
}

void CpuStrokePipeline::begin(const SolverRequest &request,
                              const AvailablePhysics &available,
                              const CpuStrokeOptions &options)
{
  if (active_ || world_.stroke_active()) {
    throw std::logic_error("A physics stroke is already active");
  }
  if (!std::isfinite(options.dt) || options.dt <= 0.0 ||
      options.substeps < 1 || options.substeps > 256 ||
      options.iterations < 1 || options.iterations > 256 ||
      options.bond_hops < 0 || options.bond_hops > 256) {
    throw std::invalid_argument("Invalid XPBD time or iteration budget");
  }
  const SolverPlan candidate = choose_solver(request, available);
  if (!candidate.implementation_available) {
    throw std::logic_error("No registered solver for this material");
  }
  if (candidate.device != SolverDevice::CPU) {
    throw std::logic_error("GPU selected but no GPU executor is wired to CpuStrokePipeline");
  }
  if (candidate.family != SolverFamily::XPBDSurface &&
      candidate.family != SolverFamily::DEMGrains) {
    throw std::logic_error("This CPU runner supports surface XPBD/approximate DEM only");
  }
  auto new_adjacency = std::make_unique<BrushAdjacency>(world_);
  world_.begin_stroke();
  adjacency_ = std::move(new_adjacency);
  options_ = options;
  plan_ = candidate;
  active_ = true;
}

CpuDabResult CpuStrokePipeline::dab(const CpuDabInput &input)
{
  if (!active_ || !world_.stroke_active()) {
    throw std::logic_error("begin() required before physical dab");
  }
  CpuDabResult result;
  result.plan = plan_;
  if (input.physics.mode == BrushPhysicsMode::Standard ||
      input.physics.influence == 0.0) {
    // Absolute bypass: do not clear / solve / modify the existing World.
    // Blender's normal brush produces the actual artistic result.
    return result;
  }

  try {
    if (input.rebuild_adjacency) {
      adjacency_ = std::make_unique<BrushAdjacency>(world_);
    }
    if (!adjacency_ || adjacency_->size() != world_.particles().size()) {
      throw std::logic_error("Physics topology changed without cache rebuild");
    }

    const bool granular = plan_.family == SolverFamily::DEMGrains;
    if (granular && !input.selection.empty() &&
        input.brush.kind != AssistedBrushKind::GranularDrag) {
      throw std::logic_error("DEM prototype accepts GranularDrag only");
    }
    if (!granular && input.brush.kind == AssistedBrushKind::GranularDrag) {
      throw std::logic_error("GranularDrag requires a DEM material");
    }
    auto goals = plan_assisted_brush(world_, *adjacency_, input.selection,
                                    input.brush, input.physics);
    if (granular) {
      std::vector<std::size_t> candidates = input.granular_candidates;
      for (const auto &s : input.selection) candidates.push_back(s.particle_index);
      std::sort(candidates.begin(), candidates.end());
      candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
      const auto contacts = plan_granular_contacts(world_, candidates, options_.granular);
      goals = merge_targets(world_, std::move(goals), contacts);
    }
    if (!world_.set_brush_targets(goals)) return result;
    world_.step_local(options_.dt, options_.substeps,
                      options_.iterations, options_.bond_hops);

    // World has advanced but editor geometry is NOT written here. Validate
    // complete Undo coverage and finite output before exposing any result.
    auto covered = input.undo_covered_vertices;
    std::sort(covered.begin(), covered.end());
    covered.erase(std::unique(covered.begin(), covered.end()), covered.end());
    for (std::size_t id : covered) {
      if (id >= world_.particles().size()) {
        throw std::out_of_range("Undo coverage contains invalid proxy ID");
      }
    }
    auto dirty = world_.dirty_particles();
    std::sort(dirty.begin(), dirty.end());
    dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    result.writes.reserve(dirty.size());
    for (const std::size_t id : dirty) {
      if (id >= world_.particles().size() ||
          !std::binary_search(covered.begin(), covered.end(), id)) {
        throw std::logic_error("XPBD wrote outside native Undo coverage");
      }
      const Vec3 position = world_.particles()[id].position;
      if (!finite(position)) {
        throw std::runtime_error("Invalid XPBD output before editor writeback");
      }
      result.writes.push_back({id, position});
    }

    if (multires_) {
      result.dirty_multires_samples = multires_->dirty_samples(dirty);
      result.dirty_multires_tiles = multires_->dirty_tiles(dirty, samples_per_tile_);
    }
    result.used_solver = true;
    return result;
  }
  catch (...) {
    // Fail closed: cancel all physics in the standalone stroke. The native
    // editor must also revert any previously written Mesh/CCG Undo nodes.
    if (active_ && world_.stroke_active()) {
      try { world_.cancel_stroke(); } catch (...) {}
    }
    active_ = false;
    adjacency_.reset();
    throw;
  }
}

void CpuStrokePipeline::commit()
{
  if (!active_ || !world_.stroke_active()) throw std::logic_error("No active stroke");
  world_.commit_stroke();
  active_ = false;
  adjacency_.reset();
}

void CpuStrokePipeline::cancel()
{
  if (!active_ || !world_.stroke_active()) throw std::logic_error("No active stroke");
  world_.cancel_stroke();
  active_ = false;
  adjacency_.reset();
}

}  // namespace sculpt_physics
