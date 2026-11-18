/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <iostream>
#include <stdexcept>
using namespace sculpt_physics;
#define CHECK(a) do {if (!(a)) throw std::runtime_error("FAILED: " #a);} while(false)
int main()
{
  // A rejected non-finite goal must not replace the previous usable brush dab.
  {
    World w;
    w.add_particle({0,0,0});
    w.add_particle({1.e308,0,0});
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1,{.25,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool threw=false;
    try {
      w.apply_grab({1.e308,0,0},1,{1.e308,0,0},
                   {BrushPhysicsMode::FullPhysics,1,0});
    }
    catch (const std::invalid_argument &) { threw=true; }
    CHECK(threw);
    // Should still apply the previous target at x=0.
    w.step_local(1./30.,1,1,0);
    CHECK(w.particles()[0].position.x > .1);
    w.cancel_stroke();
  }
  // The squared-radius fast path overflows for very large finite brushes;
  // use robust Euclidean distance in that unusual range.
  {
    World w;
    w.add_particle({1.e170,0,0});
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1.e180,{0,1,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    w.cancel_stroke();
  }
  std::cout << "Atomic Grab target validation tests passed\n";
}
