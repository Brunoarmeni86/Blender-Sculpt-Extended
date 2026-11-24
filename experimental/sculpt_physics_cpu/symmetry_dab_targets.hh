/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {
/* Candidate mirror-dab collection contract, not yet a Blender dispatch path.
 * Translations are already transformed into object space and factored by
 * native brush strength, mask and symmetry feather. Deduplicate identical
 * PBVH entries per pass; reject conflicting entries in the same pass.
 * Sum distinct passes once in canonical pass order. This overlap policy
 * must be validated against stock brush behavior before editor integration. */
struct MirrorDabSample {
  std::size_t vertex;
  unsigned pass; // XYZ mask, 0..7
  std::array<double,3> translation;
};
struct MergedMirrorTarget {
  std::size_t vertex;
  std::array<double,3> translation;
};
inline std::vector<MergedMirrorTarget> merge_mirror_dab_targets(
    std::vector<MirrorDabSample> samples, unsigned enabled_axes)
{
  if (enabled_axes>7) throw std::invalid_argument("Invalid mirror axes");
  for (const auto &sample : samples) {
    if (sample.pass>7 || (sample.pass & ~enabled_axes)!=0) {
      throw std::invalid_argument("Unexpected mirror pass");
    }
    for (double x : sample.translation) {
      if (!std::isfinite(x)) throw std::invalid_argument("Non-finite mirror target");
    }
  }
  std::sort(samples.begin(),samples.end(),[](const auto &a,const auto &b) {
    return a.vertex!=b.vertex ? a.vertex<b.vertex : a.pass<b.pass;
  });
  std::vector<MergedMirrorTarget> out;
  for (std::size_t i=0;i<samples.size();++i) {
    const auto &s=samples[i];
    if (i>0 && samples[i-1].vertex==s.vertex && samples[i-1].pass==s.pass) {
      if (samples[i-1].translation!=s.translation) {
        throw std::invalid_argument("Conflicting mirror target in one pass");
      }
      continue;
    }
    if (out.empty() || out.back().vertex!=s.vertex) out.push_back({s.vertex,{}});
    for (std::size_t axis=0;axis<3;++axis) {
      double &v=out.back().translation[axis];
      v+=s.translation[axis];
      if (!std::isfinite(v)) throw std::overflow_error("Mirror target overflow");
    }
  }
  return out;
}
} // namespace sculpt_physics
