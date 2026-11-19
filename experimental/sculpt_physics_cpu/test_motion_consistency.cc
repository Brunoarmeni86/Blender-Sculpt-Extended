/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace sculpt_physics;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("FAILED: " #x); } while (false)
static bool near(double a, double b, double eps=1.0e-8) { return std::abs(a-b)<=eps; }
static World initial_separated()
{
  World w;
  w.add_particle({0, 0, 0});
  w.add_particle({10, 0, 0});
  w.begin_stroke();
  CHECK(w.apply_grab({0,0,0}, .25, {0,.4,0}, {BrushPhysicsMode::FullPhysics,1,0}));
  w.step_local(1./30.,1,1,0);
  CHECK(w.particles()[0].velocity.y > 1.0);
  w.commit_stroke();
  w.begin_stroke();
  CHECK(w.apply_grab({10,0,0}, .25, {0,.4,0}, {BrushPhysicsMode::FullPhysics,1,0}));
  return w;
}
int main()
{
  // Distant brush dabs must not silently freeze an earlier moving component.
  {
    World global=initial_separated(), local=initial_separated(), adaptive=initial_separated();
    global.step(1./30.,1,1);
    local.step_local(1./30.,1,1,0);
    CHECK(local.last_step_statistics().used_global_fallback);
    const auto result=adaptive.step_local_adaptive(1./30.,1,1,0,1,.02);
    CHECK(result.used_global_fallback && result.accepted_hops == -1);
    for (int i=0; i<2; ++i) {
      CHECK(near(global.particles()[i].position.y, local.particles()[i].position.y));
      CHECK(near(global.particles()[i].position.y, adaptive.particles()[i].position.y));
      CHECK(near(global.particles()[i].velocity.y, local.particles()[i].velocity.y));
    }
    CHECK(global.particles()[0].position.y > .4);
    local.cancel_stroke(); adaptive.cancel_stroke(); global.cancel_stroke();
    CHECK(near(local.particles()[0].position.y, .4));
  }
  // If an old mover can be reached within max_hops, adaptively include it.
  {
    World w;
    w.add_particle({0,0,0}); w.add_particle({1,0,0}); w.add_particle({2,0,0});
    w.add_bond(0,1,10.0); w.add_bond(1,2,10.0);
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},.2,{0,.2,0},{BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,1,1,0);
    CHECK(w.particles()[0].velocity.y > 0);
    CHECK(w.apply_grab({1,0,0},.2,{0,.2,0},{BrushPhysicsMode::FullPhysics,1,0}));
    const auto outcome=w.step_local_adaptive(1./30.,1,1,0,2,.02,
                                            World::BoundaryPolicy::BoundedApproximation);
    CHECK(!outcome.used_global_fallback);
    CHECK(outcome.accepted_hops >= 1);
    CHECK(w.particles()[0].position.y > .2);
    w.cancel_stroke();
  }
  // A missed quasistatic dab stays idle, as before.
  {
    World w=initial_separated();
    CHECK(!w.apply_grab({100,0,0},.1,{0,.2,0},{BrushPhysicsMode::FullPhysics,1,0}));
    const auto previous=w.particles()[0].position.y;
    w.step_local(1./30.,1,1,0);
    CHECK(near(w.particles()[0].position.y, previous));
    CHECK(w.dirty_particles().empty());
    w.cancel_stroke();
  }
  // A failing full fallback must restore the exact pre-step physical state.
  {
    World w=initial_separated();
    const double before_position=w.particles()[0].position.y;
    bool threw=false;
    try { w.step_local(1.e308,1,1,0); }
    catch (const std::runtime_error &) { threw=true; }
    CHECK(threw);
    CHECK(near(w.particles()[0].position.y,before_position));
    w.cancel_stroke();
  }
  std::cout<<"Motion continuity tests passed\n";
}
