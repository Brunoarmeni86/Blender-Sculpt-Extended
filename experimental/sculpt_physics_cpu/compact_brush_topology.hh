/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sculpt_physics {

/* Compact, read-only edge incidence for an ordinary mesh stroke.
 * The caller owns stable edge ordering for the lifetime of this index.
 * No full-mesh particle, bond, position, velocity or journal buffers.
 * The returned IDs are global, sorted and unique; all edges incident to
 * writable vertices are retained, while fixed-fixed halo edges are omitted. */
class CompactBrushTopology {
 public:
  struct Region {
    std::vector<std::size_t> vertices;
    std::vector<std::uint32_t> edges;
  };

  template<class EdgeAt>
  CompactBrushTopology(std::size_t vertices, std::size_t edges, EdgeAt edge_at)
      : vertices_(vertices), edges_(edges)
  {
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    if (vertices >= limit || edges > limit / 2) {
      throw std::length_error("Compact brush topology exceeds 32-bit capacity");
    }
    offsets_.assign(vertices + 1, 0);
    for (std::size_t i = 0; i < edges; ++i) {
      const auto [a, b] = edge_at(i);
      validate(a, b);
      if (a == b) continue;
      ++offsets_[a + 1];
      ++offsets_[b + 1];
    }
    for (std::size_t i = 1; i < offsets_.size(); ++i) {
      offsets_[i] += offsets_[i - 1];
    }
    incident_.resize(offsets_.back());
    auto cursor = offsets_;
    for (std::size_t i = 0; i < edges; ++i) {
      const auto [a, b] = edge_at(i);
      validate(a, b);
      if (a == b) continue;
      if (cursor[a] >= offsets_[a + 1] || cursor[b] >= offsets_[b + 1]) {
        throw std::logic_error("Compact topology changed during construction");
      }
      incident_[cursor[a]++] = static_cast<std::uint32_t>(i);
      incident_[cursor[b]++] = static_cast<std::uint32_t>(i);
    }
    for (std::size_t i = 0; i < vertices; ++i) {
      if (cursor[i] != offsets_[i + 1]) {
        throw std::logic_error("Compact topology changed during construction");
      }
    }
  }

  template<class EdgeAt>
  Region gather(const std::vector<std::size_t> &active, EdgeAt edge_at) const
  {
    Region out;
    out.vertices = active;
    for (const std::size_t id : active) {
      if (id >= vertices_) throw std::out_of_range("Invalid compact brush vertex");
      for (std::uint32_t k = offsets_[id]; k < offsets_[id + 1]; ++k) {
        const std::uint32_t edge = incident_[k];
        if (edge >= edges_) throw std::logic_error("Invalid cached edge ID");
        const auto [a, b] = edge_at(edge);
        validate(a, b);
        if (a == b || (a != id && b != id)) {
          throw std::logic_error("Compact brush topology changed during stroke");
        }
        out.vertices.push_back(a == id ? b : a);
        out.edges.push_back(edge);
      }
    }
    sort_unique(out.vertices);
    sort_unique(out.edges);
    return out;
  }

  void validate_vertex(std::size_t id) const
  {
    if (id >= vertices_) throw std::out_of_range("Invalid compact brush vertex");
  }

  std::size_t storage_bytes() const
  {
    return (offsets_.capacity() + incident_.capacity()) * sizeof(std::uint32_t);
  }

 private:
  void validate(std::size_t a, std::size_t b) const
  {
    if (a >= vertices_ || b >= vertices_) {
      throw std::out_of_range("Invalid compact brush edge");
    }
  }

  template<class T> static void sort_unique(std::vector<T> &v)
  {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  }

  std::size_t vertices_;
  std::size_t edges_;
  std::vector<std::uint32_t> offsets_;
  std::vector<std::uint32_t> incident_;
};

}  // namespace sculpt_physics
