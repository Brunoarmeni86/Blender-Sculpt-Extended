/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../source/blender/editors/sculpt_paint/mesh/sculpt_sample_runs.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace blender::ed::sculpt_paint;
static int64_t checked = 0;
static void check(const std::vector<int64_t> &samples)
{
  SculptSampleRunBuilder builder;
  std::vector<SculptSampleRun> runs;
  auto emit = [&](SculptSampleRun r) { runs.push_back(r); };
  for (auto sample : samples) { builder.add(sample, emit); }
  builder.finish(emit);
  const auto count = runs.size();
  builder.finish(emit);
  if (runs.size() != count) { std::abort(); }
  std::vector<int64_t> expanded;
  for (auto run : runs) {
    if (run.size <= 0) { std::abort(); }
    for (int64_t i = 0; i < run.size; i++) { expanded.push_back(run.start + i); }
  }
  if (expanded != samples) { std::abort(); }
  checked += samples.size();
}
int main()
{
  check({}); check({0}); check({0, 1, 2, 64, 65, 63, 0, 0, 1});
  check({int64_t(1) << 34, (int64_t(1) << 34) + 1});
  std::mt19937 rng(23);
  for (int side : {2, 3, 5, 9, 17, 33, 65, 129}) {
    const int area = side * side;
    for (int repeat = 0; repeat < 20; repeat++) {
      std::vector<int64_t> samples;
      std::vector<int> grids = {4, 0, 3, 1, 2};
      std::shuffle(grids.begin(), grids.end(), rng);
      for (int grid : grids) {
        for (int i = 0; i < area; i++) {
          // Dense, alternating, random, and contiguous brush-support patterns.
          const bool changed = repeat % 4 == 0 ||
              (repeat % 4 == 1 ? i % 2 == 0 :
               repeat % 4 == 2 ? rng() % 5 == 0 : i > area / 4 && i < 3 * area / 4);
          if (changed) { samples.push_back(int64_t(grid) * area + i); }
        }
      }
      check(samples);
    }
  }
  std::printf("PASS: %lld sample IDs round-trip exactly, including reordered grids, gaps, repeats and 64-bit IDs\n", static_cast<long long>(checked));
}
