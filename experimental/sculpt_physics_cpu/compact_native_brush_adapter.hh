/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "compact_brush_topology.hh"
#include "native_grab_executor_adapter.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sculpt_physics {

/* Standalone compact adapter, shared by Blender and parity tests. It owns no
 * editor state. The caller must supply immutable stroke-start positions
 * (including vertices already moved in earlier dabs), current positions and
 * a stable edge ordering. The returned executor owns its local island.
 * Its write IDs are local: translate via local_to_global before editor writes. */
struct CompactNativeBrushDab {
  PreparedNativeBrushDab prepared;
  std::vector<std::size_t> local_to_global;
};

/* One-entry cache, owned by a native stroke. Only topology is reused;
 * current positions, rest lengths, mass, targets and lambdas are rebuilt.
 * Exact active-set equality is mandatory, even after a mask change. */
struct CompactRegionCacheV27 {
  const CompactBrushTopology *topology=nullptr;
  std::uint64_t generation=0;
  std::vector<std::size_t> active;
  CompactBrushTopology::Region region;
  std::size_t hits=0,misses=0;
};
template<class EdgeAt, class RestAt, class CurrentAt, class InverseMassAt>
CompactNativeBrushDab prepare_compact_native_brush_dab(
    const CompactBrushTopology &topology,
    const std::vector<NativeBrushTranslation> &samples,
    EdgeAt edge_at,
    RestAt rest_at,
    CurrentAt current_at,
    InverseMassAt inverse_mass_at,
    const NativeBrushDab &dab,
    bool direct_surface=false,
    CompactRegionCacheV27 *region_cache=nullptr)
{
  CompactNativeBrushDab out;
  std::vector<std::size_t> active;
  active.reserve(samples.size());
  for (const auto &sample : samples) {
    const auto &v=sample.translation;
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
      throw std::invalid_argument("Non-finite compact brush translation");
    }
    topology.validate_vertex(sample.global_index);
    const double mass=inverse_mass_at(sample.global_index);
    if (!std::isfinite(mass) || mass<0.0) {
      throw std::invalid_argument("Invalid compact target inverse mass");
    }
    if (std::hypot(v.x,v.y,v.z)>1.0e-15 && mass>0.0) {
      active.push_back(sample.global_index);
    }
  }
  std::sort(active.begin(),active.end());
  if (std::adjacent_find(active.begin(),active.end())!=active.end()) {
    throw std::invalid_argument("Duplicate compact brush target");
  }
  if (active.empty()) return out;
  // Only for already solved conservative targets. No physical executor
  // state is created: native contact, clipping, Undo coverage and writeback
  // still run in the caller, exactly as with the original compact path.
  if (direct_surface && dab.assist==0.0 && !dab.stability.enabled && !dab.clay_material) {
    out.local_to_global=active;
    ExecutorWriteBatch batch;
    batch.writes.reserve(active.size());
    for (const auto &sample:samples) {
      const auto it=std::lower_bound(active.begin(),active.end(),sample.global_index);
      if(it==active.end() || *it!=sample.global_index ||
         std::hypot(sample.translation.x,sample.translation.y,sample.translation.z)<=1e-15) continue;
      const Vec3 p=current_at(sample.global_index);
      const Vec3 goal=p+sample.translation;
      const Vec3 motion=goal-p;
      const double length=std::hypot(motion.x,motion.y,motion.z);
      if(!std::isfinite(std::hypot(p.x,p.y,p.z)) ||
         !std::isfinite(std::hypot(goal.x,goal.y,goal.z)) || !std::isfinite(length))
        throw std::invalid_argument("Invalid direct surface target");
      if(length>1e-12)batch.writes.push_back({std::size_t(it-active.begin()),goal});
    }
    std::sort(batch.writes.begin(),batch.writes.end(),[](const auto &x,const auto &y){
      return x.global_index<y.global_index;
    });
    out.prepared=PreparedNativeBrushDab(nullptr,std::move(batch));
    return out;
  }
  CompactBrushTopology::Region uncached;
  const CompactBrushTopology::Region *region=nullptr;
  if(region_cache){
    auto &cache=*region_cache;
    if(cache.topology==&topology && cache.generation==dab.topology_generation &&
       cache.active==active){
      ++cache.hits;
    }else{
      // Build before publishing the key, so a failed gather cannot leave
      // an apparently valid entry for a different active region.
      auto fresh=topology.gather(active,edge_at);
      cache.active=active;
      cache.region=std::move(fresh);
      cache.topology=&topology;
      cache.generation=dab.topology_generation;
      ++cache.misses;
    }
    region=&cache.region;
    out.local_to_global=region->vertices;
  }else{
    uncached=topology.gather(active,edge_at);
    region=&uncached;
    out.local_to_global=std::move(uncached.vertices);
  }
  const auto local_id=[&](std::size_t global) {
    const auto it=std::lower_bound(out.local_to_global.begin(),out.local_to_global.end(),global);
    if (it==out.local_to_global.end() || *it!=global) {
      throw std::logic_error("Missing compact brush vertex");
    }
    return std::size_t(it-out.local_to_global.begin());
  };
  GpuLocalIsland island;
  island.topology_generation=dab.topology_generation;
  const std::size_t n=out.local_to_global.size();
  if (n>std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("Compact island exceeds local index capacity");
  }
  island.global_ids.reserve(n);
  island.positions.reserve(n);
  island.inverse_masses.reserve(n);
  island.radii.reserve(n);
  island.writable.reserve(n);
  std::vector<Vec3> rest;
  rest.reserve(n);
  std::size_t active_cursor=0;
  for (std::size_t i=0;i<n;++i) {
    const std::size_t id=out.local_to_global[i];
    // Both lists are sorted. Keep the exact pinning policy without a
    // binary search per local vertex on the optimized path.
    while(region_cache && active_cursor<active.size() && active[active_cursor]<id)++active_cursor;
    const bool writable=region_cache ?
        active_cursor<active.size() && active[active_cursor]==id :
        std::binary_search(active.begin(),active.end(),id);
    const double mass=writable ? inverse_mass_at(id) : 0.0;
    const Vec3 p=rest_at(id);
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        !std::isfinite(mass) || mass<0.0) {
      throw std::invalid_argument("Invalid compact island rest state");
    }
    rest.push_back(p);
    island.global_ids.push_back(i);
    island.positions.push_back(current_at(id));
    island.inverse_masses.push_back(mass);
    island.radii.push_back(0.05);
    island.writable.push_back(writable);
  }
  island.bonds.reserve(region->edges.size());
  for (const auto edge_id : region->edges) {
    const auto [a,b]=edge_at(edge_id);
    const auto i=local_id(a),j=local_id(b);
    const Vec3 p=rest[i]-rest[j];
    const double length=std::hypot(p.x,p.y,p.z);
    // Match World::add_bond degenerate/overflow rejection.
    if (!std::isfinite(length) || length<1e-12 || i==j) continue;
    island.bonds.push_back({static_cast<std::uint32_t>(i),
                            static_cast<std::uint32_t>(j),length,1e-5});
  }
  std::vector<NativeBrushTranslation> local_samples;
  local_samples.reserve(active.size());
  for (const auto &sample : samples) {
    const auto &v=sample.translation;
    if (std::hypot(v.x,v.y,v.z)>1.0e-15 &&
        std::binary_search(active.begin(),active.end(),sample.global_index)) {
      local_samples.push_back({local_id(sample.global_index),sample.translation});
    }
  }
  out.prepared=prepare_cpu_native_brush_island(std::move(island),local_samples,dab);
  return out;
}

}  // namespace sculpt_physics
