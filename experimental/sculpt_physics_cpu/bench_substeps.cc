/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Comparative XPBD step schedules. A single demonstration, not a physics validation. */
#include "physics_core.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace sculpt_physics;
using Clock = std::chrono::steady_clock;

static World make_grid(const int side)
{
  World w;
  constexpr double dx = 0.075;
  for (int z = 0; z < side; ++z)
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x)
        w.add_particle({x * dx, y * dx, z * dx});
  const auto index = [side](int x, int y, int z) {
    return std::size_t((z * side + y) * side + x);
  };
  for (int z = 0; z < side; ++z)
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x) {
        if (x + 1 < side) w.add_bond(index(x, y, z), index(x + 1, y, z), 1e-5);
        if (y + 1 < side) w.add_bond(index(x, y, z), index(x, y + 1, z), 1e-5);
        if (z + 1 < side) w.add_bond(index(x, y, z), index(x, y, z + 1), 1e-5);
      }
  return w;
}

int main()
{
  for (const int side : {10, 20}) {
    const World base = make_grid(side);
    for (const int substeps : {4, 10, 20, 40}) {
      const int iterations = substeps == 4 ? 5 : substeps == 10 ? 2 :
                             substeps == 20 ? 1 : 2;
      World w = base;
      const Vec3 center{side * 0.075 / 2, side * 0.075 / 2, side * 0.075 / 2};
      w.begin_stroke();
      w.apply_grab(center, 0.18, {0, 0.1, 0},
                   {BrushPhysicsMode::PhysicalAssist, 1, 1e-6});
      const auto t0 = Clock::now();
      w.step(1.0 / 30.0, substeps, iterations);
      const auto t1 = Clock::now();
      double squared_error = 0.0;
      double max_error = 0.0;
      for (const Bond &b : w.bonds()) {
        const Vec3 p = w.particles()[b.a].position - w.particles()[b.b].position;
        const double L = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        const double error = L / b.rest_length - 1.0;
        squared_error += error * error;
        max_error = std::max(max_error, std::abs(error));
      }
      const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      std::printf("side=%d substeps=%d iterations=%d solver_ms=%.3f "
                  "rms_relative_bond_error=%.8f max_relative_bond_error=%.6f\n",
                  side, substeps, iterations, ms,
                  std::sqrt(squared_error / w.bonds().size()), max_error);
    }
  }
}
