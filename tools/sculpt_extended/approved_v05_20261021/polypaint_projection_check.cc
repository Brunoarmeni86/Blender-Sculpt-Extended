/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Numerical comparison with the original V04 formula; no Blender scene tests. */
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include "../../../source/blender/editors/sculpt_paint/mesh/sculpt_polypaint_v04_project.hh"

using RGBA = std::array<float, 4>;
using Delta = std::map<int, RGBA>;
namespace project = blender::ed::sculpt_paint::color::color_project_v04;

bool reference(const float before[4], int high, int low, int x, int y,
               const Delta &delta, double protection, int channels, float out[4])
{
  const int ratio = (high - 1) / (low - 1);
  const int lx = std::min(x / ratio, low - 2), ly = std::min(y / ratio, low - 2);
  const double a = double(x - lx * ratio) / ratio, b = double(y - ly * ratio) / ratio;
  const int indices[4] = {ly * low + lx, ly * low + lx + 1,
                         (ly + 1) * low + lx, (ly + 1) * low + lx + 1};
  const double weights[4] = {(1-a)*(1-b), a*(1-b), (1-a)*b, a*b};
  for (int c = 0; c < 4; c++) {
    if (!std::isfinite(before[c])) return false;
    out[c] = before[c];
    if (c >= channels || protection == 1) continue;
    double change = 0;
    for (int k = 0; k < 4; k++) {
      auto it = delta.find(indices[k]);
      if (it != delta.end()) change += double(it->second[c]) * weights[k];
    }
    if (!std::isfinite(change)) return false;
    if (change != 0) out[c] = float(double(before[c]) + change * (1-protection));
    if (!std::isfinite(out[c])) return false;
  }
  return true;
}

int main()
{
  std::mt19937 rng(41022);
  std::uniform_real_distribution<float> value(-1.f, 1.f);
  size_t checked = 0;
  for (int low : {2, 3, 5, 9, 17}) for (int ratio : {1, 2, 4}) {
    const int high = (low - 1) * ratio + 1;
    for (int trial = 0; trial < 12; trial++) {
      Delta delta;
      for (int i = 0; i < low * low; i++) if (rng() % 7 == 0)
        delta[i] = {value(rng), value(rng), value(rng), value(rng)};
      /* Exercise endpoints, implicit zeros, alpha preservation and signed zero. */
      delta[trial % 2 ? 0 : low * low - 1] = {value(rng), -0.f, value(rng), value(rng)};
      if (trial == 0) { delta.clear(); delta[(low/2)*low + low/2] = {.2f, -.1f, 0.f, .3f}; }
      project::Bounds bounds;
      assert(project::affected_bounds(high, low, delta, bounds));
      for (int channels : {3, 4}) for (double protection : {0., .25, 1.})
        for (int y = 0; y < high; y++) for (int x = 0; x < high; x++) {
          const float before[4] = {value(rng), -0.f, value(rng), value(rng)};
          float expected[4], actual[4];
          assert(reference(before, high, low, x, y, delta, protection, channels, expected));
          assert(project::sample(before, high, low, x, y, delta, protection, channels, actual));
          assert(std::memcmp(expected, actual, sizeof(actual)) == 0);
          if (x < bounds.x0 || x > bounds.x1 || y < bounds.y0 || y > bounds.y1)
            assert(std::memcmp(expected, before, sizeof(expected)) == 0);
          if (ratio == 1) {
            double direct[4] = {};
            auto it = delta.find(y * low + x);
            if (it != delta.end()) for (int c = 0; c < 4; c++) direct[c] = double(it->second[c]);
            assert(project::apply_delta(before, direct, protection, channels, actual));
            assert(std::memcmp(expected, actual, sizeof(actual)) == 0);
          }
          checked++;
        }
    }
  }
  std::cout << "PASS bit-identical projection samples: " << checked << '\n';
}
