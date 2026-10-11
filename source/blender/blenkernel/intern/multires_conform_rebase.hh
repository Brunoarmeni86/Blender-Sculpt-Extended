/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace blender::bke::multires_layers::conform {

/* Row-major frames; all calculations use double until displacement storage. */
struct Frame {
  double origin[3];
  double axes[3][3];
};
struct Mapping {
  double linear[3][3];
  double offset[3];
};

inline bool make_mapping(const Frame &old_frame, const Frame &new_frame, Mapping &mapping)
{
  double a[3][6]{};
  double scale = 0.0;
  for (int r = 0; r < 3; r++) {
    if (!std::isfinite(old_frame.origin[r]) || !std::isfinite(new_frame.origin[r])) {
      return false;
    }
    for (int c = 0; c < 3; c++) {
      if (!std::isfinite(old_frame.axes[r][c]) || !std::isfinite(new_frame.axes[r][c])) {
        return false;
      }
      a[r][c] = new_frame.axes[r][c];
      scale = std::max(scale, std::abs(a[r][c]));
    }
    a[r][r + 3] = 1.0;
  }
  if (scale == 0.0) {
    return false;
  }
  for (int c = 0; c < 3; c++) {
    int pivot = c;
    for (int r = c + 1; r < 3; r++) {
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) {
        pivot = r;
      }
    }
    if (std::abs(a[pivot][c]) <= scale * 1e-10) {
      return false;
    }
    for (int k = 0; k < 6; k++) {
      std::swap(a[c][k], a[pivot][k]);
    }
    const double divisor = a[c][c];
    for (int k = 0; k < 6; k++) {
      a[c][k] /= divisor;
    }
    for (int r = 0; r < 3; r++) {
      if (r == c) {
        continue;
      }
      const double factor = a[r][c];
      for (int k = 0; k < 6; k++) {
        a[r][k] -= factor * a[c][k];
      }
    }
  }
  for (int r = 0; r < 3; r++) {
    mapping.offset[r] = 0.0;
    for (int k = 0; k < 3; k++) {
      mapping.offset[r] += a[r][k + 3] * (old_frame.origin[k] - new_frame.origin[k]);
    }
    if (!std::isfinite(mapping.offset[r])) {
      return false;
    }
    for (int c = 0; c < 3; c++) {
      mapping.linear[r][c] = 0.0;
      for (int k = 0; k < 3; k++) {
        mapping.linear[r][c] += a[r][k + 3] * old_frame.axes[k][c];
      }
      if (!std::isfinite(mapping.linear[r][c])) {
        return false;
      }
    }
  }
  return true;
}

/* The base gets the origin correction. Layer vectors NEVER get that correction:
 * doing so would make preservation depend on visibility and layer strength. */
inline bool rebase(const Mapping &mapping,
                   const float input[3],
                   const bool is_base,
                   float output[3])
{
  float candidate[3];
  for (int r = 0; r < 3; r++) {
    double value = is_base ? mapping.offset[r] : 0.0;
    for (int c = 0; c < 3; c++) {
      if (!std::isfinite(input[c])) {
        return false;
      }
      value += mapping.linear[r][c] * input[c];
    }
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
      return false;
    }
    candidate[r] = float(value);
  }
  std::copy_n(candidate, 3, output);
  return true;
}

inline bool preserves(const Frame &old_frame,
                      const Frame &new_frame,
                      const float before[3],
                      const float after[3],
                      const bool is_base)
{
  double largest = 0.0, error = 0.0;
  for (int r = 0; r < 3; r++) {
    /* Compare displacement and origin difference, avoiding cancellation at
     * large world-space origins. */
    double old_value = 0.0;
    double new_value = is_base ? new_frame.origin[r] - old_frame.origin[r] : 0.0;
    for (int c = 0; c < 3; c++) {
      old_value += old_frame.axes[r][c] * before[c];
      new_value += new_frame.axes[r][c] * after[c];
    }
    if (!std::isfinite(old_value) || !std::isfinite(new_value)) {
      return false;
    }
    largest = std::max(largest, std::max(std::abs(old_value), std::abs(new_value)));
    error = std::max(error, std::abs(old_value - new_value));
  }
  return error <= 1e-6 + 1e-5 * largest;
}

}  // namespace blender::bke::multires_layers::conform
