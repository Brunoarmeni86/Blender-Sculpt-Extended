/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Assist strength is a blend coefficient, not a twice-applied Grab strength. */
#include "physics_core.hh"
#include "sculpt_surface_bridge.hh"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("FAILED: " #x); } while (false)
static bool near(double a, double b) { return std::abs(a-b) < 1e-8; }

int main()
{
  for (const double influence : {0.25, 0.5, 0.75, 1.0}) {
    World world;
    const std::vector<Vec3> rest{{0,0,0}};
    const std::vector<Vec3> artistic{{0.2,0,0}};
    world.add_particle(rest[0]);
    auto bridge = SculptSurfaceBridge::for_regular_mesh(rest, world.particles());
    world.begin_stroke();
    const BrushPhysicsSettings settings{BrushPhysicsMode::PhysicalAssist, influence, 0};
    const bool selected = influence == 0.25 ?
        world.apply_grab_candidates({0,0,0}, 1, {1,0,0}, settings, {0}) :
        world.apply_grab({0,0,0}, 1, {1,0,0}, settings);
    CHECK(selected);
    world.step_local(1.0/30, 1, 1, 0);
    CHECK(near(world.particles()[0].position.x, 1.0));
    const auto blended = bridge.compose_positions(world.particles(), artistic, settings);
    CHECK(near(blended[0].x, 0.2 * (1.0-influence) + influence));
    const auto standard = bridge.compose_positions(world.particles(), artistic,
        {BrushPhysicsMode::Standard, 1.0, 0});
    const auto disabled = bridge.compose_positions(world.particles(), artistic,
        {BrushPhysicsMode::PhysicalAssist, 0.0, 0});
    CHECK(near(standard[0].x, artistic[0].x));
    CHECK(near(disabled[0].x, artistic[0].x));
    world.cancel_stroke();
  }
  // FullPhysics uses influence as physical brush strength, not as an output blend.
  {
    World world;
    world.add_particle({0,0,0});
    world.begin_stroke();
    CHECK(world.apply_grab({0,0,0},1,{1,0,0},
          {BrushPhysicsMode::FullPhysics,0.5,0}));
    world.step_local(1.0/30,1,1,0);
    CHECK(near(world.particles()[0].position.x,0.5));
    world.cancel_stroke();
  }
  // Regular-mesh Assist, like Multires Assist, rejects invalid artistic input
  // before producing coordinates that could corrupt an edited Blender mesh.
  {
    World world;
    world.add_particle({0,0,0});
    auto bridge = SculptSurfaceBridge::for_regular_mesh({{0,0,0}},world.particles());
    bool rejected = false;
    try {
      (void)bridge.compose_positions(world.particles(),
          {{std::numeric_limits<double>::quiet_NaN(),0,0}},
          {BrushPhysicsMode::PhysicalAssist,0.5,0});
    }
    catch (const std::invalid_argument &) { rejected = true; }
    CHECK(rejected);
  }
  std::cout << "Assist strength and FullPhysics strength contracts passed\n";
}
