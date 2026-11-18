/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone diagnostic benchmark: not a performance claim about Blender. */
#include "physics_core.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace sculpt_physics;
using Clock = std::chrono::steady_clock;
namespace {
struct Timings { double snapshot_ms, selection_ms, solver_ms; std::size_t dirty; };
double milliseconds(const Clock::time_point a, const Clock::time_point b)
{
  return std::chrono::duration<double, std::milli>(b - a).count();
}
double median(std::vector<double> v)
{
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
World make_grid(const int side, const double distance)
{
  World w;
  for (int z = 0; z < side; ++z) {
    for (int y = 0; y < side; ++y) {
      for (int x = 0; x < side; ++x) {
        w.add_particle({x * distance, y * distance, z * distance});
      }
    }
  }
  const auto id = [side](const int x, const int y, const int z) {
    return std::size_t((z * side + y) * side + x);
  };
  for (int z = 0; z < side; ++z) {
    for (int y = 0; y < side; ++y) {
      for (int x = 0; x < side; ++x) {
        if (x + 1 < side) w.add_bond(id(x,y,z), id(x+1,y,z), 1.0e-5);
        if (y + 1 < side) w.add_bond(id(x,y,z), id(x,y+1,z), 1.0e-5);
        if (z + 1 < side) w.add_bond(id(x,y,z), id(x,y,z+1), 1.0e-5);
      }
    }
  }
  return w;
}
Timings run_one(World &w, int side, double spacing)
{
  const Vec3 center{side * spacing / 2, side * spacing / 2, side * spacing / 2};
  const auto t0 = Clock::now();
  w.begin_stroke();
  const auto t1 = Clock::now();
  w.apply_grab(center, 0.2, {0, 0.1, 0}, {BrushPhysicsMode::PhysicalAssist, 1, 1.e-6});
  const auto t2 = Clock::now();
  w.step(1.0 / 30.0, 4, 5);
  const auto t3 = Clock::now();
  const std::size_t dirty = w.dirty_particles().size();
  w.cancel_stroke();
  return {milliseconds(t0, t1), milliseconds(t1, t2), milliseconds(t2, t3), dirty};
}
} // namespace
int main()
{
  std::cout << "Standalone single-thread XPBD CPU benchmark; 4 substeps x 5 iterations; "
            << "same brush; 1 warmup + 5 samples; median times (ms).\n"
            << "Particle bytes=" << sizeof(Particle) << " Bond bytes=" << sizeof(Bond) << '\n';
  std::cout << "particles,bonds,snapshot_ms,brush_scan_ms,solver_ms,dirty_last\n";
  for (const int side : {10, 20, 30, 50}) {
    const double spacing = 0.075;
    World w = make_grid(side, spacing);
    run_one(w, side, spacing); // warm up, not timed.
    std::vector<double> snapshots, brushes, solvers;
    std::size_t dirty = 0;
    for (int sample = 0; sample < 5; ++sample) {
      const Timings t = run_one(w, side, spacing);
      snapshots.push_back(t.snapshot_ms);
      brushes.push_back(t.selection_ms);
      solvers.push_back(t.solver_ms);
      dirty = t.dirty;
    }
    std::cout << w.particles().size() << ',' << w.bonds().size() << ','
              << std::fixed << std::setprecision(3)
              << median(snapshots) << ',' << median(brushes) << ','
              << median(solvers) << ',' << dirty << '\n';
  }
}
