/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace blender::bke::multires_polypaint_cpu {

/** Immutable per-dab plan. No modifier, attribute lookup, allocation or resolution
 * change occurs while blending samples. The caller prepares only after the
 * legacy writer has succeeded on the first affected sample of each grid.
 * Borrowed grid pointers never survive the current node/dab. */
class PreparedPaintPlan {
  struct Axis {
    int center;
    int upper;
    int begin;
    int end;
  };
  std::vector<Axis> axes_;
  std::vector<float> weights_;
  int master_side_ = 0;
  int paint_side_ = 0;
  int step_ = 0;

  static bool valid_side(const int side)
  {
    return side >= 2 && side <= 4097 && ((side - 1) & (side - 2)) == 0;
  }

 public:
  bool prepare(const int master_side, const int paint_side)
  {
    if (!valid_side(master_side) || !valid_side(paint_side) || master_side < paint_side) {
      master_side_ = paint_side_ = step_ = 0;
      return false;
    }
    if (master_side_ == master_side && paint_side_ == paint_side) {
      return true;
    }
    master_side_ = master_side;
    paint_side_ = paint_side;
    step_ = (master_side - 1) / (paint_side - 1);
    axes_.resize(paint_side);
    for (int i = 0; i < paint_side; i++) {
      const int center = i * step_;
      axes_[i] = {center, std::min(center + 1, master_side - 1),
                  std::max(0, center - step_), std::min(master_side - 1, center + step_)};
    }
    weights_.resize(2 * step_ + 1);
    for (int i = -step_; i <= step_; i++) {
      /* Same expression as add_coarse_delta, prepared once. All coordinates
       * and scale are exactly representable powers-of-two grid positions. */
      weights_[i + step_] = std::max(0.0f, 1.0f - std::abs(float(i)) / float(step_));
    }
    return true;
  }

  /** Preserves sequential read-modify-write dependencies between coarse
   * samples. Do not batch deltas from a snapshot or parallelize overlapping
   * writes within this grid. Finite HDR/negative RGB values are retained. */
  bool blend(float (*colors)[4], const int x, const int y,
             const float primary[3], const float influence) const
  {
    if (!colors || !primary || step_ == 0 || x < 0 || y < 0 ||
        x >= paint_side_ || y >= paint_side_ ||
        !std::isfinite(influence) || influence <= 0.0f || influence > 1.0f) {
      return false;
    }
    const Axis &ax = axes_[x];
    const Axis &ay = axes_[y];
    const int center_index = ay.center * master_side_ + ax.center;
    float before[4];
    for (int c = 0; c < 4; c++) {
      /* Keep the legacy bilinear read at integer master coordinates, including
       * its behavior around non-finite neighbors. The writer used to repeat
       * this identical read before computing the delta. */
      const float a = colors[center_index][c] * 1.0f +
                      colors[ay.center * master_side_ + ax.upper][c] * 0.0f;
      const float b = colors[ay.upper * master_side_ + ax.center][c] * 1.0f +
                      colors[ay.upper * master_side_ + ax.upper][c] * 0.0f;
      before[c] = a * 1.0f + b * 0.0f;
    }
    float delta[4];
    for (int c = 0; c < 3; c++) {
      const float target = before[c] * (1.0f - influence) + primary[c] * influence;
      delta[c] = target - before[c];
      if (!std::isfinite(target) || !std::isfinite(delta[c])) {
        return false;
      }
    }
    if (!std::isfinite(before[3])) {
      return false;
    }
    delta[3] = before[3] - before[3];
    if (step_ == 1) {
      /* Only the center has non-zero support. Keep legacy +=(target-before)
       * rounding instead of assigning target directly. The eight surrounding
       * zero-weight stores can only affect signs of zero in finite inputs. */
      for (int c = 0; c < 4; c++) {
        colors[center_index][c] += delta[c];
      }
      return true;
    }
    for (int my = ay.begin; my <= ay.end; my++) {
      const float wy = weights_[my - ay.center + step_];
      if (wy == 0.0f) {
        continue;
      }
      for (int mx = ax.begin; mx <= ax.end; mx++) {
        const float wx = weights_[mx - ax.center + step_];
        if (wx == 0.0f) {
          continue;
        }
        const float weight = wx * wy;
        for (int c = 0; c < 4; c++) {
          colors[my * master_side_ + mx][c] += weight * delta[c];
        }
      }
    }
    return true;
  }
};

}  // namespace blender::bke::multires_polypaint_cpu
