/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "compact_brush_topology.hh"
#include "native_grab_executor_adapter.hh"

#include <algorithm>
#include <cmath>
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
    if (std::hypot(v.x,v.y,v.z)>1.0e-15 &&
        inverse_mass_at(sample.global_index)>0.0) {
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
  World local_world;
  local_world.reserve_topology(out.local_to_global.size(),region.edges.size());
  for (const std::size_t id : out.local_to_global) {
    local_world.add_particle(rest_at(id),std::binary_search(active.begin(),active.end(),id) ?
                                         inverse_mass_at(id) : 0.0);
  }
  for (const auto edge_id : region.edges) {
    const auto [a,b]=edge_at(edge_id);
    try {
      local_world.add_bond(local_id(a),local_id(b),1.0e-5);
    }
    catch (const std::invalid_argument &) {
      // Same degenerate-rest-edge policy as the full World reference.
    }
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
  const GpuTopologyBondIndex local_topology(local_world,dab.topology_generation);
  out.prepared=prepare_cpu_native_brush_dab(
      local_world,local_topology,local_samples,
      [&](std::size_t id) { return current_at(out.local_to_global.at(id)); },dab);
  return out;
}

}  // namespace sculpt_physics
