/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone CPU solver-step benchmark, NOT Blender viewport timing. */
#include "physics_core.hh"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace sculpt_physics;
using Clock = std::chrono::steady_clock;

static World make_scene(const std::size_t inactive_particles)
{
  World w;
  // Two independent moving components with three particles and two bonds each.
  for (int i = 0; i < 3; ++i) w.add_particle({double(i), 0, 0});
  for (int i = 0; i < 3; ++i) w.add_particle({100 + double(i), 0, 0});
  for (int i = 0; i < 2; ++i) {
    w.add_bond(i, i + 1, 1.e-5);
    w.add_bond(i + 3, i + 4, 1.e-5);
  }
  for (std::size_t i = 0; i < inactive_particles; ++i) {
    w.add_particle({double(i), 300, 300});
  }
  w.begin_stroke();
  w.apply_grab({0, 0, 0}, 0.3, {0, 0.2, 0},
               {BrushPhysicsMode::FullPhysics, 1, 0});
  w.step_local_multi(1. / 30., 2, 5, 3);
  w.commit_stroke();
  w.begin_stroke();
  w.apply_grab({100, 0, 0}, 0.3, {0, 0.3, 0},
               {BrushPhysicsMode::FullPhysics, 1, 0});
  return w;
}

int main()
{
  std::cout << "particles,mode,median_step_ms,integrated,global_fallback\n";
  for (const std::size_t inactive : {1000u, 27000u, 125000u}) {
    const World baseline = make_scene(inactive);
    for (int mode : {0, 1, 2}) {
      std::vector<double> times;
      std::size_t integrated = 0;
      bool fallback = false;
      for (int i = 0; i < 7; ++i) {
        World w = baseline; // clone OUTSIDE the timed solver step.
        const auto begin = Clock::now();
        if (mode == 0) w.step(1. / 30., 2, 5);
        else if (mode == 1) w.step_local(1. / 30., 2, 5, 3);
        else w.step_local_multi(1. / 30., 2, 5, 3);
        const auto end = Clock::now();
        if (i > 0) {
          times.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        }
        integrated = w.last_step_statistics().integrated_particles;
        fallback = w.last_step_statistics().used_global_fallback;
      }
      std::sort(times.begin(), times.end());
      std::cout << baseline.particles().size() << ','
                << (mode == 0 ? "global" : mode == 1 ? "legacy-local" : "multi-local")
                << ',' << std::fixed << std::setprecision(3) << (times[2] + times[3]) / 2
                << ',' << integrated << ',' << fallback << '\n';
    }
  }
}
