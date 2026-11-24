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

template<class EdgeAt, class RestAt, class CurrentAt, class InverseMassAt>
CompactNativeBrushDab prepare_compact_native_brush_dab(
    const CompactBrushTopology &topology,
    const std::vector<NativeBrushTranslation> &samples,
    EdgeAt edge_at,
    RestAt rest_at,
    CurrentAt current_at,
    InverseMassAt inverse_mass_at,
    const NativeBrushDab &dab)
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
  auto region=topology.gather(active,edge_at);
  out.local_to_global=std::move(region.vertices);
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
  for (std::size_t i=0;i<n;++i) {
    const std::size_t id=out.local_to_global[i];
    const bool writable=std::binary_search(active.begin(),active.end(),id);
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
  island.bonds.reserve(region.edges.size());
  for (const auto edge_id : region.edges) {
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
