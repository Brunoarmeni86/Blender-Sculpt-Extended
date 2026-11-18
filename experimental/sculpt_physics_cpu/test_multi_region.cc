/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace sculpt_physics;

#define CHECK(condition) \
  do { \
    if (!(condition)) throw std::runtime_error("FAILED: " #condition); \
  } while (false)

static bool near(double a, double b, double e = 1.e-7)
{
  return std::abs(a - b) < e;
}

static World two_islands()
{
  World w;
  // Two disconnected, independent three-particle components.
  for (int i = 0; i < 3; ++i) w.add_particle({double(i), 0, 0});
  for (int i = 0; i < 3; ++i) w.add_particle({100 + double(i), 0, 0});
  for (int i = 0; i < 2; ++i) {
    w.add_bond(i, i + 1, 1.e-5);
    w.add_bond(i + 3, i + 4, 1.e-5);
  }
  w.begin_stroke();
  CHECK(w.apply_grab({0, 0, 0}, 0.3, {0, 0.2, 0},
                     {BrushPhysicsMode::FullPhysics, 1, 0}));
  w.step_local_multi(1. / 30., 2, 5, 3);
  w.commit_stroke();

  w.begin_stroke();
  CHECK(w.apply_grab({100, 0, 0}, 0.3, {0, 0.3, 0},
                     {BrushPhysicsMode::FullPhysics, 1, 0}));
  return w;
}

int main()
{
  // The second stroke must update BOTH components, not force the global solver.
  {
    World reference = two_islands(), multi = two_islands(), legacy = two_islands();
    const double before_stroke = multi.particles()[0].position.y;
    reference.step(1. / 30., 2, 5);
    multi.step_local_multi(1. / 30., 2, 5, 3);
    legacy.step_local(1. / 30., 2, 5, 3);
    CHECK(!multi.last_step_statistics().used_global_fallback);
    CHECK(legacy.last_step_statistics().used_global_fallback);
    CHECK(multi.last_step_statistics().integrated_particles == 6);
    CHECK(multi.particles()[0].position.y > before_stroke);
    for (int i = 0; i < 6; ++i) {
      CHECK(near(multi.particles()[i].position.y, reference.particles()[i].position.y));
      CHECK(near(multi.particles()[i].velocity.y, reference.particles()[i].velocity.y));
    }
    multi.cancel_stroke();
    reference.cancel_stroke();
    legacy.cancel_stroke();
    CHECK(near(multi.particles()[0].position.y, before_stroke));
  }
  // A regular second dab does not require global fallback for multiple islands.
  {
    World w = two_islands();
    w.step_local_multi(1. / 30., 1, 4, 1);
    CHECK(!w.last_step_statistics().used_global_fallback);
    w.cancel_stroke();
  }
  // Gravity and ground contact still use global simulation deliberately.
  {
    World w = two_islands();
    w.set_gravity({0, -9.81, 0});
    w.step_local_multi(1. / 30., 1, 3, 1);
    CHECK(w.last_step_statistics().used_global_fallback);
    w.cancel_stroke();
  }
  // A missed quasistatic dab remains idle: no unintended background simulation.
  {
    World w = two_islands();
    CHECK(!w.apply_grab({999, 0, 0}, 0.1, {0, 1, 0},
                        {BrushPhysicsMode::FullPhysics, 1, 0}));
    const Vec3 previous = w.particles()[0].position;
    w.step_local_multi(1. / 30., 1, 3, 2);
    CHECK(w.dirty_particles().empty());
    CHECK(near(w.particles()[0].position.y, previous.y));
    w.cancel_stroke();
  }
  std::cout << "Multi-island Sculpt physics tests passed\n";
}
