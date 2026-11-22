/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_executor.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sculpt_physics {
namespace {

bool finite(Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

double length(Vec3 p)
{
  return std::hypot(p.x,p.y,p.z);
}

void validate_island(const GpuLocalIsland &island)
{
  const std::size_t n=island.global_ids.size();
  if (island.crossing_bonds != 0) {
    throw std::logic_error("Executor refuses incomplete physical halo");
  }
  if (n != island.positions.size() || n != island.inverse_masses.size() ||
      n != island.writable.size() || n != island.radii.size()) {
    throw std::invalid_argument("Executor island attribute count mismatch");
  }
  for (std::size_t i=0;i<n;++i) {
    if (i>0 && island.global_ids[i-1]>=island.global_ids[i]) {
      throw std::invalid_argument("Executor requires unique sorted global proxy IDs");
    }
    if (!finite(island.positions[i]) ||
        !std::isfinite(island.inverse_masses[i]) ||
        island.inverse_masses[i]<0.0 ||
        !std::isfinite(island.radii[i]) || island.radii[i]<=0.0) {
      throw std::invalid_argument("Non-finite executor proxy particle");
    }
    if (!island.writable[i] && island.inverse_masses[i] != 0.0) {
      throw std::logic_error("Non-Undo-covered halo particle is not pinned");
    }
  }
  for (const GpuLocalBond &b : island.bonds) {
    if (b.a>=n || b.b>=n || b.a==b.b ||
        !std::isfinite(b.rest_length) || b.rest_length<=0.0 ||
        !std::isfinite(b.compliance) || b.compliance<0.0) {
      throw std::invalid_argument("Invalid executor bond");
    }
  }
}

class CpuXpbdExecutor final : public IPhysicsExecutor {
 public:
  ExecutorCapabilities capabilities() const override
  {
    return {SolverDevice::CPU,SolverFamily::XPBDSurface,true,true};
  }

  void allocate_island(const GpuLocalIsland &island) override
  {
    if (active_) throw std::logic_error("Cannot replace an active stroke island");
    validate_island(island);
    // Strong exception safety: copy and allocate first, then take ownership.
    GpuLocalIsland staged = island;
    std::vector<Vec3> positions = island.positions;
    std::vector<Vec3> acknowledged = island.positions;
    // Allocate sparse tracking once per stable proxy topology, never per dab.
    std::vector<unsigned char> dirty_flags(island.positions.size(),0);
    std::vector<std::size_t> dirty_ids;
    dirty_ids.reserve(island.positions.size());
    island_ = std::move(staged);
    positions_.swap(positions);
    acknowledged_.swap(acknowledged);
    dirty_flags_.swap(dirty_flags);
    dirty_ids_.swap(dirty_ids);
    revision_ = 0;
    snapshot_.clear();
    targets_.clear();
    ready_ = true;
  }

  std::uint64_t topology_generation() const override
  {
    if (!ready_) throw std::logic_error("Executor has no allocated island");
    return island_.topology_generation;
  }

  bool stroke_active() const override { return active_; }

  void begin_stroke() override
  {
    if (!ready_ || active_) throw std::logic_error("Executor stroke already active or no island");
    std::vector<Vec3> new_snapshot = positions_;
    std::vector<Vec3> acknowledged = positions_;
    snapshot_.swap(new_snapshot);
    acknowledged_.swap(acknowledged);
    for (std::size_t i : dirty_ids_) dirty_flags_[i]=0;
    dirty_ids_.clear();
    revision_=0;
    targets_.clear();
    active_=true;
  }

  void upload_dirty(const std::vector<ExecutorEdit> &edits) override
  {
    require_active();
    std::vector<std::pair<std::size_t,Vec3>> accepted;
    accepted.reserve(edits.size());
    for (const ExecutorEdit &e : edits) {
      const std::size_t id = local_id(e.global_index);
      if (!island_.writable[id] || island_.inverse_masses[id] <= 0.0) {
        throw std::logic_error("Cannot edit pinned or Undo-uncovered vertex");
      }
      if (!finite(e.position)) throw std::invalid_argument("Non-finite edit");
      accepted.emplace_back(id,e.position);
    }
    std::sort(accepted.begin(),accepted.end(),
              [](const auto &a,const auto &b){return a.first<b.first;});
    for (std::size_t i=1;i<accepted.size();++i) {
      if (accepted[i-1].first==accepted[i].first) {
        throw std::invalid_argument("Duplicate sparse edit ID");
      }
    }
    if (!accepted.empty() && revision_==std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Executor writeback revision exhausted");
    }
    // No allocations/no exceptions after all input has been validated.
    for (const auto &edit : accepted) {
      positions_[edit.first] = edit.second;
      mark_dirty(edit.first);
    }
    if (!accepted.empty()) ++revision_;
  }

  void set_targets(const std::vector<ExecutorTarget> &targets) override
  {
    require_active();
    std::vector<LocalTarget> validated;
    validated.reserve(targets.size());
    for (const ExecutorTarget &target : targets) {
      const std::size_t id=local_id(target.global_index);
      if (!island_.writable[id] || island_.inverse_masses[id]<=0.0) {
        throw std::logic_error("Cannot target pinned/Undo-excluded vertex");
      }
      if (!finite(target.goal) ||
          !std::isfinite(target.compliance) || target.compliance<0.0) {
        throw std::invalid_argument("Invalid XPBD target");
      }
      validated.push_back({id,target.goal,target.compliance});
    }
    std::sort(validated.begin(),validated.end(),
              [](const auto &a,const auto &b){return a.id<b.id;});
    for (std::size_t i=1;i<validated.size();++i) {
      if (validated[i-1].id==validated[i].id) {
        throw std::invalid_argument("Duplicate XPBD target ID");
      }
    }
    targets_.swap(validated);
  }

  void step(double dt, int substeps, int iterations) override
  {
    require_active();
    if (!std::isfinite(dt) || dt<=0.0 || substeps<1 || substeps>256 ||
        iterations<1 || iterations>256) {
      throw std::invalid_argument("Invalid XPBD step budget");
    }
    const double h=dt/static_cast<double>(substeps);
    if (!std::isfinite(h) || h<=0.0 ||
        h*h<std::numeric_limits<double>::min()) {
      throw std::invalid_argument("Invalid XPBD substep square");
    }
    if (targets_.empty()) return; // Local sculpt dab missed the proxy.

    // Transactional per dab: errors never leak half-solved geometry.
    // Reuse allocations across dabs. Positions are still copied into a
    // separate scratch buffer so a failed solve cannot corrupt the stroke.
    scratch_positions_.assign(positions_.begin(),positions_.end());
    scratch_bond_lambda_.assign(island_.bonds.size(),0.0);
    scratch_target_lambda_.assign(targets_.size(),Vec3{});
    auto &next=scratch_positions_;
    auto &bond_lambda=scratch_bond_lambda_;
    auto &target_lambda=scratch_target_lambda_;
    for (int sub=0;sub<substeps;++sub) {
      std::fill(bond_lambda.begin(),bond_lambda.end(),0.0);
      for (auto &lambda : target_lambda) lambda={};
      for (int iter=0;iter<iterations;++iter) {
        for (std::size_t t=0;t<targets_.size();++t) {
          const LocalTarget &target=targets_[t];
          const double w=island_.inverse_masses[target.id];
          const double alpha=target.compliance/(h*h);
          const double denominator=w+alpha;
          if (!std::isfinite(alpha) || !std::isfinite(denominator) ||
              denominator<=0.0) throw std::runtime_error("Target XPBD denominator invalid");
          const Vec3 c=next[target.id]-target.goal;
          const Vec3 delta=(c*(-1.0)-target_lambda[t]*alpha)/denominator;
          if (!finite(delta)) throw std::runtime_error("Target XPBD diverged");
          next[target.id]+=delta*w;
          target_lambda[t]+=delta;
          if (!finite(next[target.id]) || !finite(target_lambda[t])) {
            throw std::runtime_error("Target position/multiplier overflow");
          }
        }
        for (std::size_t k=0;k<island_.bonds.size();++k) {
          const GpuLocalBond &bond=island_.bonds[k];
          Vec3 diff=next[bond.b]-next[bond.a];
          const double dist=length(diff);
          if (!std::isfinite(dist)) throw std::runtime_error("Bond distance overflow");
          if (dist<=1e-12) continue;
          const double alpha=bond.compliance/(h*h);
          const double w0=island_.inverse_masses[bond.a];
          const double w1=island_.inverse_masses[bond.b];
          const double denominator=w0+w1+alpha;
          if (!std::isfinite(alpha) || !std::isfinite(denominator)) {
            throw std::runtime_error("Bond XPBD denominator overflow");
          }
          if (denominator<=0.0) continue; // All particles fixed.
          const double delta=(-(dist-bond.rest_length)-alpha*bond_lambda[k])/denominator;
          if (!std::isfinite(delta) || !std::isfinite(bond_lambda[k]+delta)) {
            throw std::runtime_error("Bond XPBD multiplier overflow");
          }
          bond_lambda[k]+=delta;
          diff=diff/dist;
          next[bond.a]+=diff*(-w0*delta);
          next[bond.b]+=diff*(w1*delta);
          if (!finite(next[bond.a]) || !finite(next[bond.b])) {
            throw std::runtime_error("Bond XPBD position overflow");
          }
        }
      }
    }
    if (revision_==std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Executor writeback revision exhausted");
    }
    for (std::size_t i=0;i<positions_.size();++i) {
      if (next[i].x != positions_[i].x ||
          next[i].y != positions_[i].y ||
          next[i].z != positions_[i].z) {
        mark_dirty(i);
      }
    }
    positions_.swap(next);
    ++revision_;
  }

  std::vector<GpuProxyChange> download_dirty(double epsilon) const override
  {
    require_active();
    return stage_gpu_local_writes(island_,positions_,epsilon);
  }

  ExecutorWriteBatch stage_incremental(double epsilon) const override
  {
    require_active();
    if (!std::isfinite(epsilon) || epsilon < 0.0 ||
        acknowledged_.size()!=positions_.size()) {
      throw std::invalid_argument("Invalid incremental writeback threshold/state");
    }
    ExecutorWriteBatch batch;
    batch.revision = revision_;
    for (std::size_t i : dirty_ids_) {
      const Vec3 current=positions_[i];
      const Vec3 old=acknowledged_[i];
      if (!finite(current) || !finite(old)) {
        throw std::invalid_argument("Non-finite incremental XPBD writeback");
      }
      const double movement=length(current-old);
      if (!std::isfinite(movement)) {
        throw std::invalid_argument("Incremental XPBD displacement overflow");
      }
      if (movement<=epsilon) continue;
      if (!island_.writable[i] || island_.inverse_masses[i]<=0.0) {
        throw std::logic_error("Incremental XPBD moved an untracked/pinned vertex");
      }
      batch.writes.push_back({island_.global_ids[i],current});
    }
    std::sort(batch.writes.begin(),batch.writes.end(),
              [](const GpuProxyChange &a,const GpuProxyChange &b) {
                return a.global_index < b.global_index;
              });
    return batch;
  }

  void acknowledge_incremental(std::uint64_t revision) override
  {
    require_active();
    if (revision != revision_) {
      throw std::logic_error("Stale editor writeback acknowledgment");
    }
    // All storage is reserved when the island is allocated. Updating an
    // acknowledged dab now costs O(changed K), NOT O(all island N).
    for (std::size_t i : dirty_ids_) {
      acknowledged_[i]=positions_[i];
      dirty_flags_[i]=0;
    }
    dirty_ids_.clear();
  }

  void commit() override
  {
    require_active();
    // Positions and baseline are equal-length, so this assignment does not
    // require reallocation. Editor MUST have written an Undo-covered transaction.
    island_.positions=positions_;
    targets_.clear();
    snapshot_.clear();
    active_=false;
  }

  void cancel() override
  {
    require_active();
    positions_.swap(snapshot_);
    snapshot_.clear();
    targets_.clear();
    active_=false;
  }

 private:
  struct LocalTarget {
    std::size_t id=0;
    Vec3 goal{};
    double compliance=0.0;
  };

  void mark_dirty(std::size_t local_id)
  {
    // dirty_ids_ has N slots reserved during allocate_island. Each local ID
    // is inserted at most once before the next native editor acknowledgment.
    if (!dirty_flags_[local_id]) {
      dirty_flags_[local_id]=1;
      dirty_ids_.push_back(local_id);
    }
  }

  void require_active() const
  {
    if (!active_) throw std::logic_error("Executor begin_stroke required");
  }

  std::size_t local_id(std::size_t global_id) const
  {
    const auto it=std::lower_bound(island_.global_ids.begin(),
                                   island_.global_ids.end(),global_id);
    if (it==island_.global_ids.end() || *it!=global_id) {
      throw std::out_of_range("Proxy global ID missing from physics island");
    }
    return static_cast<std::size_t>(it-island_.global_ids.begin());
  }

  GpuLocalIsland island_;
  std::vector<Vec3> positions_;
  std::vector<Vec3> snapshot_;
  std::vector<Vec3> acknowledged_;
  // Per-dab simulation scratch, never the live committed/reference state.
  // Buffer capacities persist across repeated strokes and local solver steps.
  std::vector<Vec3> scratch_positions_;
  std::vector<double> scratch_bond_lambda_;
  std::vector<Vec3> scratch_target_lambda_;
  std::vector<unsigned char> dirty_flags_;
  std::vector<std::size_t> dirty_ids_;
  std::uint64_t revision_=0;
  std::vector<LocalTarget> targets_;
  bool ready_=false;
  bool active_=false;
};
}  // namespace

std::unique_ptr<IPhysicsExecutor> make_cpu_xpbd_executor()
{
  return std::make_unique<CpuXpbdExecutor>();
}

std::unique_ptr<IPhysicsExecutor> make_native_executor(SolverDevice device)
{
  if (device != SolverDevice::CPU) {
    throw std::logic_error("Native GPU executor unavailable: no CUDA/Vulkan bridge installed");
  }
  return make_cpu_xpbd_executor();
}


std::unique_ptr<IPhysicsExecutor> make_native_executor(const SolverPlan &plan)
{
  if (!plan.implementation_available || plan.device != SolverDevice::CPU ||
      plan.family != SolverFamily::XPBDSurface) {
    throw std::logic_error("No verified native executor for requested solver family/device");
  }
  return make_cpu_xpbd_executor();
}

}  // namespace sculpt_physics
