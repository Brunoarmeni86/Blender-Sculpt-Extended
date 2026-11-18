/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace sculpt_physics;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("FAILED: " #x); } while(false)
static bool near(double a, double b, double eps = 1e-7)
{
  return std::abs(a - b) < eps;
}
int main()
{
  // Expand the local island when a movable artificial boundary is disturbed.
  {
    World chain;
    for (int i = 0; i < 9; ++i) chain.add_particle({double(i), 0, 0});
    for (int i = 0; i < 8; ++i) chain.add_bond(i, i + 1, 0.0);
    chain.begin_stroke();
    CHECK(chain.apply_grab({4,0,0}, .2, {0,1,0},
                           {BrushPhysicsMode::FullPhysics,1,0}));
    const auto result = chain.step_local_adaptive(1./30., 2, 8, 0, 8, 0.0);
    CHECK(!result.used_global_fallback);
    CHECK(result.attempts > 1 && result.accepted_hops > 0);
    CHECK(chain.particles()[4].position.y > 0);
    CHECK(chain.apply_grab({4,0,0}, 1.5, {0,-.2,0},
                           {BrushPhysicsMode::FullPhysics,1,0}));
    CHECK(chain.step_local_adaptive(1./30.,2,8,0,8,0.0).attempts == 1);
    chain.cancel_stroke();
  }
  // Failure at the maximum halo rolls back the local candidate before
  // calling the global solver. Bond plastic rest changes must also roll back.
  {
    auto prepare = []() {
      World w;
      for (int i = 0; i < 7; ++i) w.add_particle({double(i),0,0});
      for (int i = 0; i < 6; ++i) w.add_bond(i,i+1,0,.03,3.0,100.0);
      w.begin_stroke();
      w.apply_grab({3,0,0},.2,{0,1,0},
                   {BrushPhysicsMode::FullPhysics,1,0});
      return w;
    };
    World baseline=prepare(), adaptive=prepare();
    baseline.step(1./30.,2,6);
    const auto r=adaptive.step_local_adaptive(1./30.,2,6,0,0,0.0);
    CHECK(r.used_global_fallback && r.accepted_hops == -1);
    for (int i=0;i<7;++i) {
      CHECK(near(adaptive.particles()[i].position.x,baseline.particles()[i].position.x));
      CHECK(near(adaptive.particles()[i].position.y,baseline.particles()[i].position.y));
    }
    for (int i=0;i<6;++i) {
      CHECK(near(adaptive.bonds()[i].rest_length,baseline.bonds()[i].rest_length));
      CHECK(adaptive.bonds()[i].broken==baseline.bonds()[i].broken);
    }
    baseline.cancel_stroke(); adaptive.cancel_stroke();
  }
  // A bounded mode exposes unconverged approximation and equals the direct
  // solver evaluated on its accepted last halo (no duplicate motion).
  {
    auto prepare = [](){
      World w;
      for(int i=0;i<9;++i)w.add_particle({double(i),0,0});
      for(int i=0;i<8;++i)w.add_bond(i,i+1,0);
      w.begin_stroke();
      w.apply_grab({4,0,0},.2,{0,1,0},
                   {BrushPhysicsMode::FullPhysics,1,0});
      return w;
    };
    World fixed=prepare(),bounded=prepare();
    fixed.step_local(1./30.,2,8,2);
    const auto r=bounded.step_local_adaptive(
       1./30.,2,8,0,2,0.0,World::BoundaryPolicy::BoundedApproximation);
    CHECK(!r.used_global_fallback && !r.converged && r.accepted_hops==2);
    for(int i=0;i<9;++i) {
      CHECK(near(fixed.particles()[i].position.y,bounded.particles()[i].position.y));
    }
    fixed.cancel_stroke(); bounded.cancel_stroke();
  }
  // Overflow in the solver must not poison the next dab's world state.
  {
    World w;
    w.add_particle({0,0,0});
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1.0,{1.e308,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool threw=false;
    try { (void) w.step_local_adaptive(1./30.,1,1,0,0,0.0); }
    catch (const std::runtime_error &) { threw=true; }
    CHECK(threw);
    CHECK(near(w.particles()[0].position.x,0));
    CHECK(near(w.particles()[0].velocity.x,0));
    CHECK(w.dirty_particles().empty());
    w.cancel_stroke();
  }
  std::cout << "Adaptive solver tests passed\n";
}
