/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu_workset.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;

namespace {
using Clock = std::chrono::steady_clock;

double percentile(std::vector<double> sorted, double quantile)
{
  if (sorted.empty()) return 0.0;
  std::sort(sorted.begin(), sorted.end());
  const std::size_t n = sorted.size();
  return sorted[static_cast<std::size_t>((n - 1) * quantile)];
}

void report(const char *name, const std::vector<double> &latency)
{
  std::cout << name << " p50_ms=" << std::fixed << std::setprecision(4)
            << percentile(latency, 0.5) << " p95_ms="
            << percentile(latency, 0.95) << "\n";
}
}  // namespace

int main(int argc, char **argv)
{
  // Controlled, deterministic CPU workset benchmark, NOT GPU runtime.
  const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 125000;
  const int dabs = argc > 2 ? std::atoi(argv[2]) : 100;
  if (count < 100 || count > 2000000 || dabs < 2 || dabs > 10000) {
    throw std::invalid_argument("Usage: bench_gpu_workset [vertices 100..2M] [dabs 2..10000]");
  }
  World world;
  for (std::size_t i = 0; i < count; ++i) {
    world.add_particle({static_cast<double>(i), 0.0, 0.0});
    if (i > 0) world.add_bond(i-1,i);
  }

  const auto setup_start = Clock::now();
  GpuTopologyBondIndex index(world, 1);
  const auto setup_end = Clock::now();
  const double build_ms =
      std::chrono::duration<double, std::milli>(setup_end - setup_start).count();
  std::vector<double> full_times;
  std::vector<double> cached_times;
  full_times.reserve(dabs);
  cached_times.reserve(dabs);
  std::size_t touched_bonds = 0;

  // Warm both paths once, use identical centers and hold the same workset.
  for (int run = -1; run < dabs; ++run) {
    const std::size_t center = 20 + static_cast<std::size_t>(
        static_cast<unsigned int>(std::max(0, run)) * 997U % (count - 40));
    const std::vector<std::size_t> active{center};
    std::vector<std::size_t> halo;
    for (std::size_t p = center - 8; p <= center + 8; ++p) {
      if (p != center) halo.push_back(p);
    }
    const auto s1 = Clock::now();
    auto full = build_gpu_local_island(world,active,halo,active,1,true);
    const auto s2 = Clock::now();
    auto cached = index.build(world,active,halo,active,1,true);
    const auto s3 = Clock::now();
    if (full.global_ids != cached.global_ids ||
        full.bonds.size() != cached.bonds.size() ||
        full.crossing_bonds != cached.crossing_bonds) {
      throw std::runtime_error("Indexed island differs from full scan");
    }
    touched_bonds += cached.bonds.size();
    if (run >= 0) {
      full_times.push_back(std::chrono::duration<double, std::milli>(s2-s1).count());
      cached_times.push_back(std::chrono::duration<double, std::milli>(s3-s2).count());
    }
  }
  std::cout << "CPU local GPU island preparation (not GPU execution)\n"
            << "vertices=" << count << " bonds=" << world.bonds().size()
            << " dabs=" << dabs << " selected_per_dab=17"
            << " index_build_ms=" << std::fixed << std::setprecision(4)
            << build_ms << " local_bonds_total=" << touched_bonds << "\n";
  report("full_scan",full_times);
  report("cached_CSR",cached_times);
  return 0;
}
