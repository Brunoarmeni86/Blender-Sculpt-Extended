/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "multires_dirty_map.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sculpt_physics {

MultiresDirtyMap::MultiresDirtyMap(std::size_t proxy_count,
                                   const std::vector<SurfaceAnchor> &anchors)
{
  if (proxy_count == std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("Invalid proxy vertex count");
  }
  offsets_.assign(proxy_count + 1, 0);
  stamps_.assign(anchors.size(), 0);
  for (const SurfaceAnchor &anchor : anchors) {
    for (int k = 0; k < 3; ++k) {
      if (anchor.vertices[k] >= proxy_count ||
          !std::isfinite(anchor.weights[k]) || anchor.weights[k] < 0.0) {
        throw std::invalid_argument("Invalid Multires anchor");
      }
    }
    if (std::abs(anchor.weights[0] + anchor.weights[1] + anchor.weights[2] - 1.0) >
        1.0e-6) {
      throw std::invalid_argument("Unnormalized Multires anchor");
    }
    for (int k = 0; k < 3; ++k) {
      bool first = true;
      for (int earlier = 0; earlier < k; ++earlier) {
        if (anchor.vertices[earlier] == anchor.vertices[k] &&
            anchor.weights[earlier] > 0.0) first = false;
      }
      if (first && anchor.weights[k] > 0.0) ++offsets_[anchor.vertices[k] + 1];
    }
  }
  for (std::size_t i = 1; i < offsets_.size(); ++i) offsets_[i] += offsets_[i - 1];
  sample_ids_.resize(offsets_.back());
  std::vector<std::size_t> cursor = offsets_;
  for (std::size_t sample = 0; sample < anchors.size(); ++sample) {
    const SurfaceAnchor &anchor = anchors[sample];
    for (int k = 0; k < 3; ++k) {
      bool first = true;
      for (int earlier = 0; earlier < k; ++earlier) {
        if (anchor.vertices[earlier] == anchor.vertices[k] &&
            anchor.weights[earlier] > 0.0) first = false;
      }
      if (first && anchor.weights[k] > 0.0) {
        sample_ids_[cursor[anchor.vertices[k]]++] = sample;
      }
    }
  }
}

std::vector<std::size_t> MultiresDirtyMap::dirty_samples(
    const std::vector<std::size_t> &dirty_proxy)
{
  // Validate before touching the generation and scratch stamps.
  for (std::size_t i : dirty_proxy) {
    if (i >= offsets_.size() - 1) throw std::out_of_range("Dirty proxy vertex OOB");
  }
  if (++generation_ == 0) {
    std::fill(stamps_.begin(), stamps_.end(), 0);
    generation_ = 1;
  }
  std::vector<std::size_t> result;
  for (std::size_t vertex : dirty_proxy) {
    for (std::size_t j = offsets_[vertex]; j < offsets_[vertex + 1]; ++j) {
      const std::size_t sample = sample_ids_[j];
      if (stamps_[sample] == generation_) continue;
      stamps_[sample] = generation_;
      result.push_back(sample);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::size_t> MultiresDirtyMap::dirty_tiles(
    const std::vector<std::size_t> &dirty_proxy, std::size_t samples_per_tile)
{
  if (samples_per_tile == 0) throw std::invalid_argument("Invalid tile size");
  const auto samples = dirty_samples(dirty_proxy);
  std::vector<std::size_t> tiles;
  tiles.reserve(samples.size());
  for (std::size_t sample : samples) {
    const std::size_t tile = sample / samples_per_tile;
    if (tiles.empty() || tiles.back() != tile) tiles.push_back(tile);
  }
  return tiles;
}

}  // namespace sculpt_physics
