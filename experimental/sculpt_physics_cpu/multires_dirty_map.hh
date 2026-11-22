/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sculpt_physics {

/* Sparse inverse correspondence between proxy vertices and evaluated Multires
 * samples. No CCG writeback: caller owns the Multires topology/layer/Undo gate.
 * UV/color attributes remain per face corner in the source geometry. */
class MultiresDirtyMap {
 public:
  MultiresDirtyMap(std::size_t proxy_count,
                   const std::vector<SurfaceAnchor> &sample_anchors);

  std::vector<std::size_t> dirty_samples(const std::vector<std::size_t> &dirty_proxy);
  std::vector<std::size_t> dirty_tiles(const std::vector<std::size_t> &dirty_proxy,
                                       std::size_t samples_per_tile);
  std::size_t sample_count() const { return stamps_.size(); }

 private:
  std::vector<std::size_t> offsets_;
  std::vector<std::size_t> sample_ids_;
  std::vector<std::uint32_t> stamps_;
  std::uint32_t generation_ = 0;
};

}  // namespace sculpt_physics
