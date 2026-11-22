/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_grab_executor_adapter.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace sculpt_physics {
namespace {

bool finite(Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

}  // namespace

PreparedNativeGrabDab::PreparedNativeGrabDab(
    std::unique_ptr<IPhysicsExecutor> executor,
    ExecutorWriteBatch batch)
    : executor_(std::move(executor)), batch_(std::move(batch))
{
}

PreparedNativeGrabDab::~PreparedNativeGrabDab()
{
  cancel();
}

void PreparedNativeGrabDab::finish_editor_success()
{
  if (!executor_) return; // Empty brush dab is an intentional no-op.
  executor_->acknowledge_incremental(batch_.revision);
  executor_->commit();
  executor_.reset();
}

void PreparedNativeGrabDab::cancel() noexcept
{
  if (!executor_) return;
  try {
    if (executor_->stroke_active()) executor_->cancel();
  }
  catch (...) {
    // Never throw from an RAII destructor on an editor failure path.
  }
  executor_.reset();
}

PreparedNativeGrabDab prepare_cpu_native_grab_dab(
    const World &rest_world,
    const GpuTopologyBondIndex &topology,
    const std::vector<NativeGrabSample> &samples,
    const std::function<Vec3(std::size_t)> &current_position,
    const NativeGrabDab &dab)
{
  if (!current_position || !finite(dab.delta) ||
      !std::isfinite(dab.compliance) || dab.compliance < 0.0 ||
      !std::isfinite(dab.dt) || dab.dt <= 0.0 ||
      dab.substeps < 1 || dab.substeps > 256 ||
      dab.iterations < 1 || dab.iterations > 256) {
    throw std::invalid_argument("Invalid native Grab executor settings");
  }

  std::vector<NativeGrabSample> selected;
  selected.reserve(samples.size());
  for (const NativeGrabSample &sample : samples) {
    if (sample.global_index >= rest_world.particles().size() ||
        !std::isfinite(sample.factor) ||
        sample.factor < 0.0 || sample.factor > 1.0) {
      throw std::invalid_argument("Invalid native Grab PBVH factor/index");
    }
    if (sample.factor > 0.0) selected.push_back(sample);
  }
  std::sort(selected.begin(), selected.end(),
            [](const NativeGrabSample &a, const NativeGrabSample &b) {
              return a.global_index < b.global_index;
            });
  for (std::size_t i=1; i<selected.size(); ++i) {
    if (selected[i].global_index == selected[i-1].global_index) {
      throw std::invalid_argument("Duplicate Grab vertex: normalize PBVH ownership first");
    }
  }
  // Even empty worksets check topology generation to avoid retaining a
  // stale GPU/CPU proxy when Blender mutates mesh topology.
  if (selected.empty()) {
    (void)topology.one_ring_halo(rest_world,{},dab.topology_generation);
    return {};
  }

  std::vector<std::size_t> active;
  active.reserve(selected.size());
  for (const auto &sample : selected) active.push_back(sample.global_index);
  const auto halo = topology.one_ring_halo(rest_world, active, dab.topology_generation);
  GpuLocalIsland island = topology.build(rest_world, active, halo, active,
                                         dab.topology_generation, false);
  // Only FREE active vertices can move. Every halo vertex is pinned so that
  // omitted bonds crossing a fixed halo remain mathematically harmless.
  for (std::size_t i=0; i<island.global_ids.size(); ++i) {
    const Vec3 p=current_position(island.global_ids[i]);
    if (!finite(p)) throw std::invalid_argument("Non-finite native Grab mesh position");
    island.positions[i]=p;
  }

  std::vector<ExecutorTarget> targets;
  targets.reserve(selected.size());
  for (const auto &sample : selected) {
    const auto it=std::lower_bound(island.global_ids.begin(),
                                    island.global_ids.end(),sample.global_index);
    if (it==island.global_ids.end() || *it!=sample.global_index) {
      throw std::logic_error("Missing local Grab vertex after island construction");
    }
    const std::size_t local=static_cast<std::size_t>(it-island.global_ids.begin());
    if (island.inverse_masses[local]<=0.0) continue;
    const Vec3 goal=island.positions[local]+dab.delta*sample.factor;
    if (!finite(goal)) throw std::invalid_argument("Grab displacement overflow");
    targets.push_back({sample.global_index,goal,dab.compliance});
  }
  if (targets.empty()) return {};

  auto executor=make_native_executor(SolverDevice::CPU);
  executor->allocate_island(island);
  executor->begin_stroke();
  executor->set_targets(targets);
  executor->step(dab.dt,dab.substeps,dab.iterations);
  ExecutorWriteBatch batch=executor->stage_incremental();
  // For a first-ring pinning policy, all writes must be inside the
  // explicitly selected active set. No unrecorded exterior moves allowed.
  for (const GpuProxyChange &write : batch.writes) {
    if (!std::binary_search(active.begin(),active.end(),write.global_index)) {
      throw std::logic_error("Native Grab XPBD wrote outside PBVH active/Undo set");
    }
  }
  return PreparedNativeGrabDab(std::move(executor),std::move(batch));
}

}  // namespace sculpt_physics
