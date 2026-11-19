/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace sculpt_physics;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("FAILED: " #c); } while (false)

int main()
{
  // A rigid target should be independent of the particle's nonzero
  // inverse-mass scale: the prior absolute 1e-12 denominator cutoff froze it.
  for(int method=0; method<3; ++method) {
    World w;
    w.add_particle({0,0,0},1e-14);
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1,{.25,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    if(method==0) w.step(1./30.,1,1);
    else if(method==1) w.step_local(1./30.,1,1,0);
    else w.step_local_multi(1./30.,1,1,0);
    CHECK(std::abs(w.particles()[0].position.x-.25) < 1e-10);
    w.cancel_stroke();
    CHECK(w.particles()[0].position.x == 0);
  }
  // Both add_bond() and global XPBD must support distances representable in
  // double even if squaring their components would overflow the float range.
  {
    World w;
    w.add_particle({0,0,0});
    w.add_particle({1e155,0,0});
    w.add_bond(0,1,1);
    w.begin_stroke();
    w.step(1./30.,1,1);
    CHECK(std::isfinite(w.particles()[1].position.x));
    CHECK(std::abs(w.particles()[1].position.x-1e155) < 1e140);
    w.cancel_stroke();
  }
  // PBVH-like unordered, duplicate candidate list must yield the same
  // constraint order and physical result as full-scanning all particles.
  {
    World full, indexed;
    for (int i=0; i<10000; ++i) {
      const double x=i*.01;
      full.add_particle({x,0,0});
      indexed.add_particle({x,0,0});
    }
    full.begin_stroke(); indexed.begin_stroke();
    const BrushPhysicsSettings cfg{BrushPhysicsMode::FullPhysics,1,0};
    CHECK(full.apply_grab({5,0,0},.025,{0,.3,0},cfg));
    CHECK(indexed.apply_grab_candidates({5,0,0},.025,{0,.3,0},cfg,
                                        {502,499,500,501,499,499,498}));
    for (int n=0;n<2;++n) {
      full.step_local(1./30.,1,2,0);
      indexed.step_local(1./30.,1,2,0);
      for (std::size_t i=0; i<full.particles().size();++i) {
        CHECK(full.particles()[i].position.y == indexed.particles()[i].position.y);
        CHECK(full.particles()[i].velocity.y == indexed.particles()[i].velocity.y);
      }
      if(n==0) {
        bool threw=false;
        try { indexed.apply_grab_candidates({5,0,0},.025,{0,0,0},cfg,{500,10000}); }
        catch(const std::out_of_range &) { threw=true; }
        CHECK(threw); // Previous valid target must survive invalid candidate.
      }
    }
    CHECK(!indexed.apply_grab_candidates({0,0,0},1,{0,.5,0},
                                         {BrushPhysicsMode::Standard,1,0},{999999}));
    full.cancel_stroke(); indexed.cancel_stroke();
  }
  std::cout << "Foundation scale / indexed brush query tests passed\n";
}
