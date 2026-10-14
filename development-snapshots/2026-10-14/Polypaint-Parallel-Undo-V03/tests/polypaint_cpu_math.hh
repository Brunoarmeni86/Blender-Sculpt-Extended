/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Multires Polypaint V0.2a: CPU interpolation and coarse-edit transfer.
 * CCG UV coordinates: (0,0) face center, (1,0) next edge,
 * (0,1) previous edge, (1,1) original mesh corner.
 * This header depends on no Blender runtime so its exact math can be tested
 * with sanitizers before attempting a Blender Windows compile. */
#include <algorithm>
#include <cmath>

namespace blender::bke::multires_polypaint_cpu {

inline bool valid_side(int side)
{
  return side >= 2 && side <= 4097 && ((side - 1) & (side - 2)) == 0;
}

/* Base patch order: center / forward edge / backward edge / corner. */
inline void sample_patch(const float patch[4][4], const float u, const float v, float out[4])
{
  const float s = std::clamp(u, 0.0f, 1.0f);
  const float t = std::clamp(v, 0.0f, 1.0f);
  for (int c = 0; c < 4; c++) {
    const float a = patch[0][c] * (1.0f - s) + patch[1][c] * s;
    const float b = patch[2][c] * (1.0f - s) + patch[3][c] * s;
    out[c] = a * (1.0f - t) + b * t;
  }
}

inline bool sample_grid(const float (*colors)[4],
                        const int side,
                        const float u,
                        const float v,
                        float out[4])
{
  if (!out || !colors || !valid_side(side) || !std::isfinite(u) || !std::isfinite(v)) {
    return false;
  }
  const float fx = std::clamp(u, 0.0f, 1.0f) * float(side - 1);
  const float fy = std::clamp(v, 0.0f, 1.0f) * float(side - 1);
  const int x0 = int(fx), y0 = int(fy);
  const int x1 = std::min(x0 + 1, side - 1), y1 = std::min(y0 + 1, side - 1);
  const float dx = fx - x0, dy = fy - y0;
  for (int c = 0; c < 4; c++) {
    const float a = colors[y0 * side + x0][c] * (1.0f - dx) +
                    colors[y0 * side + x1][c] * dx;
    const float b = colors[y1 * side + x0][c] * (1.0f - dx) +
                    colors[y1 * side + x1][c] * dx;
    out[c] = a * (1.0f - dy) + b * dy;
  }
  return true;
}

/* Add an interpolated low-frequency difference into an already-persisted
 * high-resolution grid. The high-frequency residual is not resampled or
 * flattened: only an additive local correction is introduced. */
inline bool add_coarse_delta(float (*colors)[4],
                             const int master_side,
                             const int paint_side,
                             const int paint_x,
                             const int paint_y,
                             const float delta[4])
{
  if (!colors || !delta || !valid_side(master_side) || !valid_side(paint_side) ||
      master_side < paint_side ||
      paint_x < 0 || paint_y < 0 || paint_x >= paint_side || paint_y >= paint_side)
  {
    return false;
  }
  for (int c = 0; c < 4; c++) {
    if (!std::isfinite(delta[c])) {
      return false;
    }
  }
  const float scale = float(master_side - 1) / float(paint_side - 1);
  const float cx = float(paint_x) * scale, cy = float(paint_y) * scale;
  const int x_start = std::max(0, int(std::floor(cx - scale)));
  const int x_end = std::min(master_side - 1, int(std::ceil(cx + scale)));
  const int y_start = std::max(0, int(std::floor(cy - scale)));
  const int y_end = std::min(master_side - 1, int(std::ceil(cy + scale)));
  for (int y = y_start; y <= y_end; y++) {
    const float wy = std::max(0.0f, 1.0f - std::abs(float(y) - cy) / scale);
    for (int x = x_start; x <= x_end; x++) {
      const float wx = std::max(0.0f, 1.0f - std::abs(float(x) - cx) / scale);
      const float w = wx * wy;
      for (int c = 0; c < 4; c++) {
        colors[y * master_side + x][c] += w * delta[c];
      }
    }
  }
  return true;
}
}  // namespace blender::bke::multires_polypaint_cpu
