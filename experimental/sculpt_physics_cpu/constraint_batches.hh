/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {
/* Deterministic greedy edge coloring. Each group contains disjoint endpoints,
 * including pinned endpoints, so an in-place kernel cannot race even when
 * it writes zero corrections. Input order is stable; colors may change the
 * Gauss-Seidel convergence path and require numerical parity evaluation. */
struct ConstraintBatches {
  std::vector<std::size_t> offsets;
  std::vector<std::uint32_t> bond_ids;
};

template<class EdgeAt>
ConstraintBatches build_constraint_batches(std::size_t vertices,
                                          std::size_t bonds,
                                          EdgeAt edge_at)
{
  if (bonds>std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("Too many local constraints");
  }
  std::vector<std::vector<std::uint32_t>> used(vertices);
  std::vector<std::uint32_t> color(bonds);
  std::vector<std::size_t> marks;
  std::vector<std::size_t> counts;
  for (std::size_t i=0;i<bonds;++i) {
    const auto [a,b]=edge_at(i);
    if (a>=vertices || b>=vertices || a==b) {
      throw std::invalid_argument("Invalid constraint endpoint");
    }
    const std::size_t stamp=i+1;
    for (const auto c : used[a]) marks[c]=stamp;
    for (const auto c : used[b]) marks[c]=stamp;
    std::size_t c=0;
    while (c<marks.size() && marks[c]==stamp) ++c;
    if (c==marks.size()) {
      marks.push_back(0);
      counts.push_back(0);
    }
    color[i]=static_cast<std::uint32_t>(c);
    used[a].push_back(color[i]);
    used[b].push_back(color[i]);
    ++counts[c];
  }
  ConstraintBatches out;
  out.offsets.resize(counts.size()+1,0);
  for (std::size_t c=0;c<counts.size();++c) out.offsets[c+1]=out.offsets[c]+counts[c];
  out.bond_ids.resize(bonds);
  auto cursor=out.offsets;
  for (std::size_t i=0;i<bonds;++i) {
    out.bond_ids[cursor[color[i]]++]=static_cast<std::uint32_t>(i);
  }
  return out;
}
} // namespace sculpt_physics
