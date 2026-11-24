/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_grab_executor_adapter.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sculpt_physics {
namespace {

bool finite(Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

void validate_material(const NativeMaterialResponse &material)
{
  if (!std::isfinite(material.bond_compliance_scale) ||
      material.bond_compliance_scale<0.0 ||
      !std::isfinite(material.native_rest_follow) ||
      material.native_rest_follow<0.0 || material.native_rest_follow>1.0) {
    throw std::invalid_argument("Invalid native material response");
  }
}

/* Alter only the isolated local island, never the persistent rest World.
 * Native goals already contain the artist's mask, falloff and pressure.
 * Unselected/fixed halo points retain their current coordinates. */
void apply_material(GpuLocalIsland &island,
                    const std::vector<ExecutorTarget> &goals,
                    const NativeMaterialResponse &material)
{
  if (material.bond_compliance_scale==1.0 && material.native_rest_follow==0.0) return;
  std::vector<Vec3> native_positions;
  if (material.native_rest_follow>0.0) {
    native_positions=island.positions;
    for (const auto &goal : goals) {
      const auto it=std::lower_bound(island.global_ids.begin(),island.global_ids.end(),
                                     goal.global_index);
      if (it==island.global_ids.end() || *it!=goal.global_index) {
        throw std::logic_error("Missing material goal");
      }
      native_positions[std::size_t(it-island.global_ids.begin())]=goal.goal;
    }
  }
  for (auto &bond : island.bonds) {
    bond.compliance*=material.bond_compliance_scale;
    if (!std::isfinite(bond.compliance)) {
      throw std::overflow_error("Native bond compliance overflow");
    }
    if (material.native_rest_follow==0.0 ||
        (island.inverse_masses[bond.a]==0.0 && island.inverse_masses[bond.b]==0.0)) continue;
    const Vec3 d=native_positions[bond.b]-native_positions[bond.a];
    const double length=std::hypot(d.x,d.y,d.z);
    if (!std::isfinite(length)) throw std::overflow_error("Native rest length overflow");
    // Keep strictly positive rest lengths even for coincident native goals.
    const double floor=std::max(std::numeric_limits<double>::min(),bond.rest_length*1e-6);
    const double follow=material.native_rest_follow;
    bond.rest_length=(1.0-follow)*bond.rest_length+follow*std::max(length,floor);
    if (!std::isfinite(bond.rest_length) || bond.rest_length<=0.0) {
      throw std::overflow_error("Invalid adapted native rest length");
    }
  }
}

// Local yield/pressure approximation. The native targets remain authoritative
// for assist=0. No new vertices, corner edits, volume or contact simulation.
// Adapt rest lengths after yield and redistribute differential brush pressure
// along existing edges, only between writable vertices.
void apply_clay_material(GpuLocalIsland &island,
                         std::vector<ExecutorTarget> &goals,
                         const NativeBrushDab &dab)
{
  const std::size_t n=island.positions.size();
  const std::size_t missing=std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> target(n,missing);
  std::vector<Vec3> motion(n), spread(n);
  std::vector<double> pressure(n,0.0);
  std::vector<unsigned int> degree(n,0);
  const auto norm=[](Vec3 v) { return std::hypot(v.x,v.y,v.z); };
  for (std::size_t k=0;k<goals.size();++k) {
    const auto it=std::lower_bound(island.global_ids.begin(),island.global_ids.end(),
                                   goals[k].global_index);
    const auto i=std::size_t(it-island.global_ids.begin());
    target[i]=k;
    motion[i]=goals[k].goal-island.positions[i];
    pressure[i]=norm(motion[i]);
  }
  for (auto &bond : island.bonds) {
    const auto a=bond.a,b=bond.b;
    if (target[a]==missing && target[b]==missing) continue;
    const Vec3 edge=island.positions[b]-island.positions[a];
    const double length=norm(edge);
    const double predicted=norm(edge+motion[b]-motion[a]);
    if (!std::isfinite(length) || !std::isfinite(predicted)) {
      throw std::overflow_error("Clay material edge overflow");
    }
    // Current geometry carries previous deformation. This is a per-dab
    // yield approximation, NOT a stored constitutive plastic-strain tensor.
    const double rest=bond.rest_length;
    const double strain=(predicted-rest)/rest;
    const double excess=std::max(0.0,std::abs(strain)-dab.clay_yield);
    const double change=std::copysign(std::min(excess,0.25),strain);
    bond.rest_length=rest*(1.0+dab.clay_plasticity*change);
    if (target[a]==missing || target[b]==missing || length<=1e-12) continue;
    // Equal/opposite pressure redistribution along the surface edge.
    // Project away the local mean brush-motion direction to favor lateral
    // flow. Bound every pair by the smaller native displacement.
    Vec3 tangent=edge*(1.0/length);
    const Vec3 mean=motion[a]+motion[b];
    const double mean_len=norm(mean);
    if (mean_len>1e-12) {
      const Vec3 normal=mean*(1.0/mean_len);
      const double d=tangent.x*normal.x+tangent.y*normal.y+tangent.z*normal.z;
      tangent=tangent-normal*d;
    }
    const double tlen=norm(tangent);
    if (tlen<=1e-12) continue;
    const double amount=dab.clay_spread*std::clamp(
        pressure[a]-pressure[b],-0.5*std::min(pressure[a],pressure[b]),
                                 0.5*std::min(pressure[a],pressure[b]));
    const Vec3 offset=tangent*(amount/tlen);
    spread[a]=spread[a]-offset;
    spread[b]=spread[b]+offset;
    ++degree[a]; ++degree[b];
  }
  for (std::size_t i=0;i<n;++i) {
    if (target[i]==missing || degree[i]==0) continue;
    const Vec3 correction=spread[i]*(1.0/double(degree[i]));
    goals[target[i]].goal=goals[target[i]].goal+correction;
    if (!finite(goals[target[i]].goal)) throw std::overflow_error("Clay goal overflow");
  }
}

}  // namespace

NativeBrushProfile experimental_native_brush_profile(NativeSculptBrushKind kind)
{
  // These are qualitative trial profiles, not measured material constants.
  // A high native_rest_follow is deliberately used for additive/smoothing
  // tools whose intended geometry must not be pulled back to the old shape.
  switch (kind) {
    case NativeSculptBrushKind::Clay:       return {2e-7,{2.0,0.85}};
    case NativeSculptBrushKind::ClayStrips: return {1e-7,{1.5,0.95}};
    case NativeSculptBrushKind::Smooth:     return {2e-7,{1.0,0.95}};
    case NativeSculptBrushKind::Draw:       return {1e-7,{2.0,0.90}};
    case NativeSculptBrushKind::Nudge:      return {5e-7,{1.0,0.50}};
    case NativeSculptBrushKind::Gravity:    return {2e-6,{0.5,0.20}};
  }
  throw std::invalid_argument("Unsupported native brush profile");
}

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

  validate_material(dab.material);
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
  if (selected.empty() ||
      (dab.delta.x==0.0 && dab.delta.y==0.0 && dab.delta.z==0.0)) {
    (void)topology.one_ring_halo(rest_world,{},dab.topology_generation);
    return {};
  }

  std::vector<std::size_t> active;
  active.reserve(selected.size());
  for (const auto &sample : selected) {
    if (rest_world.particles()[sample.global_index].inverse_mass>0.0) {
      active.push_back(sample.global_index);
    }
  }
  const auto halo = topology.one_ring_halo(rest_world, active, dab.topology_generation);
  // Fully masked/hidden selection is a no-op before packing or reading
  // editor positions. Pinned samples can still be included as actual halo.
  if (active.empty()) return {};
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
    if (rest_world.particles()[sample.global_index].inverse_mass<=0.0) continue;
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
  apply_material(island,targets,dab.material);

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

namespace {
void validate_native_brush_dab(const NativeBrushDab &dab)
{
  // Validate first. This shared path must not multiply existing native
  // Blender brush factors or silently substitute other material solvers.
  if (!std::isfinite(dab.assist) ||
      dab.assist<0.0 || dab.assist>1.0 ||
      !std::isfinite(dab.compliance) ||
      dab.compliance<0.0 || !std::isfinite(dab.dt) || dab.dt<=0.0 ||
      dab.substeps<1 || dab.substeps>256 ||
      dab.iterations<1 || dab.iterations>256) {
    throw std::invalid_argument("Invalid native physical brush settings");
  }
  switch (dab.kind) {
    case NativeSculptBrushKind::Clay:
    case NativeSculptBrushKind::ClayStrips:
    case NativeSculptBrushKind::Smooth:
    case NativeSculptBrushKind::Draw:
    case NativeSculptBrushKind::Nudge:
    case NativeSculptBrushKind::Gravity:
      break;
    default:
      throw std::invalid_argument("Unsupported native physical brush kind");
  }
  validate_material(dab.material);
  if (!std::isfinite(dab.clay_yield) || dab.clay_yield<0.0 || dab.clay_yield>1.0 ||
      !std::isfinite(dab.clay_plasticity) || dab.clay_plasticity<0.0 || dab.clay_plasticity>1.0 ||
      !std::isfinite(dab.clay_spread) || dab.clay_spread<0.0 || dab.clay_spread>1.0) {
    throw std::invalid_argument("Invalid Clay V2 settings");
  }
}
} // namespace

PreparedNativeBrushDab prepare_cpu_native_brush_dab(
    const World &rest_world,
    const GpuTopologyBondIndex &topology,
    const std::vector<NativeBrushTranslation> &samples,
    const std::function<Vec3(std::size_t)> &current_position,
    const NativeBrushDab &dab)
{
  if (!current_position) throw std::invalid_argument("Missing current position callback");
  validate_native_brush_dab(dab);
  std::vector<NativeBrushTranslation> selected;
  selected.reserve(samples.size());
  for (const NativeBrushTranslation &sample : samples) {
    if (sample.global_index>=rest_world.particles().size() ||
        !finite(sample.translation)) {
      throw std::invalid_argument("Invalid native sculpt translation or vertex ID");
    }
    if (std::hypot(sample.translation.x,sample.translation.y,
                   sample.translation.z)>1.0e-15) {
      selected.push_back(sample);
    }
  }
  std::sort(selected.begin(),selected.end(),
            [](const NativeBrushTranslation &a,const NativeBrushTranslation &b) {
              return a.global_index < b.global_index;
            });
  for (std::size_t i=1;i<selected.size();++i) {
    if (selected[i-1].global_index==selected[i].global_index) {
      throw std::invalid_argument("Duplicate native brush translation vertex ID");
    }
  }
  std::vector<std::size_t> active;
  active.reserve(selected.size());
  for (const auto &sample : selected) {
    if (rest_world.particles()[sample.global_index].inverse_mass>0.0) {
      active.push_back(sample.global_index);
    }
  }
  // Always validate cached topology, including when there is no dab.
  const auto halo=topology.one_ring_halo(
      rest_world,active,dab.topology_generation);
  if (active.empty()) return {};

  GpuLocalIsland island=topology.build(
      rest_world,active,halo,active,dab.topology_generation,false);
  for (std::size_t i=0;i<island.global_ids.size();++i) {
    const Vec3 position=current_position(island.global_ids[i]);
    if (!finite(position)) {
      throw std::invalid_argument("Non-finite current Blender mesh position");
    }
    island.positions[i]=position;
  }
  selected.erase(std::remove_if(selected.begin(),selected.end(),
      [&](const auto &sample) {
        return !std::binary_search(active.begin(),active.end(),sample.global_index);
      }),selected.end());
  return prepare_cpu_native_brush_island(std::move(island),selected,dab);
}

PreparedNativeBrushDab prepare_cpu_native_brush_island(
    GpuLocalIsland island,
    const std::vector<NativeBrushTranslation> &samples,
    const NativeBrushDab &dab)
{
  validate_native_brush_dab(dab);
  const std::size_t n=island.global_ids.size();
  if (island.topology_generation!=dab.topology_generation ||
      island.positions.size()!=n || island.inverse_masses.size()!=n ||
      island.writable.size()!=n || island.radii.size()!=n ||
      island.crossing_bonds!=0 ||
      !std::is_sorted(island.global_ids.begin(),island.global_ids.end()) ||
      std::adjacent_find(island.global_ids.begin(),island.global_ids.end())!=island.global_ids.end()) {
    throw std::invalid_argument("Invalid native brush island");
  }
  std::vector<NativeBrushTranslation> selected;
  selected.reserve(samples.size());
  for (const auto &sample : samples) {
    if (!finite(sample.translation)) throw std::invalid_argument("Invalid native translation");
    if (!std::binary_search(island.global_ids.begin(),island.global_ids.end(),sample.global_index)) {
      throw std::out_of_range("Native target outside prepared island");
    }
    if (std::hypot(sample.translation.x,sample.translation.y,sample.translation.z)>1e-15) {
      selected.push_back(sample);
    }
  }
  std::sort(selected.begin(),selected.end(),
            [](const auto &a,const auto &b) {return a.global_index<b.global_index;});
  for (std::size_t i=1;i<selected.size();++i) {
    if (selected[i-1].global_index==selected[i].global_index)
      throw std::invalid_argument("Duplicate island translation");
  }
  std::vector<std::size_t> active;
  active.reserve(selected.size());
  for (std::size_t i=0;i<n;++i) {
    if (!finite(island.positions[i]) || !std::isfinite(island.inverse_masses[i]) ||
        island.inverse_masses[i]<0.0 || !std::isfinite(island.radii[i]) ||
        island.radii[i]<=0.0 || (!island.writable[i] && island.inverse_masses[i]!=0.0)) {
      throw std::invalid_argument("Invalid native island particle");
    }
    if (island.inverse_masses[i]>0.0) active.push_back(island.global_ids[i]);
  }
  for (const auto &bond : island.bonds) {
    if (bond.a>=n || bond.b>=n || bond.a==bond.b ||
        !std::isfinite(bond.rest_length) || bond.rest_length<=0.0 ||
        !std::isfinite(bond.compliance) || bond.compliance<0.0)
      throw std::invalid_argument("Invalid native island bond");
  }
  std::vector<ExecutorTarget> goals;
  goals.reserve(active.size());
  for (const auto &sample : selected) {
    if (!std::binary_search(active.begin(),active.end(),sample.global_index)) continue;
    const auto it=std::lower_bound(
        island.global_ids.begin(),island.global_ids.end(),sample.global_index);
    if (it==island.global_ids.end() || *it!=sample.global_index) {
      throw std::logic_error("Missing native sculpt target inside local physics island");
    }
    const std::size_t local=static_cast<std::size_t>(it-island.global_ids.begin());
    const Vec3 goal=island.positions[local]+sample.translation;
    if (!finite(goal)) throw std::invalid_argument("Native brush goal overflow");
    goals.push_back({sample.global_index,goal,dab.compliance});
  }
  if (goals.size()!=active.size()) throw std::logic_error("Mobile island vertex lacks native target");
  if (goals.empty()) return {};
  const bool clay=dab.assist>0.0 && dab.clay_material &&
      (dab.kind==NativeSculptBrushKind::Clay || dab.kind==NativeSculptBrushKind::ClayStrips);
  const auto native_goals=clay && dab.assist<1.0 ? goals : std::vector<ExecutorTarget>{};
  if (dab.assist>0.0) {
    if (clay) {
      apply_clay_material(island,goals,dab);
      // Do not erase the yielded rest lengths with the legacy rest-follow.
      apply_material(island,goals,{dab.material.bond_compliance_scale,0.0});
    }
    else apply_material(island,goals,dab.material);
  }

  auto executor=dab.colored_order ? make_cpu_colored_xpbd_executor() :
                                    make_native_executor(SolverDevice::CPU);
  executor->allocate_island(island);
  executor->begin_stroke();
  if (dab.assist>0.0) {
    executor->set_targets(goals);
    executor->step(dab.dt,dab.substeps,dab.iterations);
  }
  if (dab.assist<1.0) {
    // Preserve the artist's fully factored native displacement as baseline.
    // Missing physical writes mean the vertex stayed at its initial position.
    const auto physical=executor->stage_incremental(0.0);
    std::vector<ExecutorEdit> blended;
    blended.reserve(goals.size());
    std::size_t changed=0;
    const auto &blend_goals=native_goals.empty() ? goals : native_goals;
    for (const auto &goal : blend_goals) {
      while (changed<physical.writes.size() &&
             physical.writes[changed].global_index<goal.global_index) {
        ++changed;
      }
      const auto it=std::lower_bound(island.global_ids.begin(),island.global_ids.end(),
                                     goal.global_index);
      if (it==island.global_ids.end() || *it!=goal.global_index) {
        throw std::logic_error("Missing V15 blend target");
      }
      const auto local=std::size_t(it-island.global_ids.begin());
      const Vec3 solved=changed<physical.writes.size() &&
                        physical.writes[changed].global_index==goal.global_index ?
                        physical.writes[changed].position : island.positions[local];
      const Vec3 position=dab.assist==0.0 ? goal.goal :
          goal.goal*(1.0-dab.assist)+solved*dab.assist;
      if (!finite(position)) throw std::runtime_error("Non-finite V15 blended target");
      blended.push_back({goal.global_index,position});
    }
    // Acknowledge the actual blended editor positions, not the raw solve.
    executor->upload_dirty(blended);
  }
  ExecutorWriteBatch batch=executor->stage_incremental();
  for (const GpuProxyChange &write:batch.writes) {
    if (!std::binary_search(active.begin(),active.end(),write.global_index)) {
      throw std::logic_error("Native physical brush changed vertex outside editor Undo set");
    }
  }
  return PreparedNativeBrushDab(std::move(executor),std::move(batch));
}

}  // namespace sculpt_physics
