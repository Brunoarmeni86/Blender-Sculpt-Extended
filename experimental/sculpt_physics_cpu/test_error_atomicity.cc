/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace sculpt_physics;
#define CHECK(a) do {if (!(a)) throw std::runtime_error("FAILED: " #a);} while(false)

int main()
{
  // A failed numeric solve must not leave a half-updated particle in any mode.
  for (int mode=0; mode<3; ++mode) {
    World w;
    w.add_particle({0,0,0});
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1.0,{1.e308,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool threw=false;
    try {
      if(mode==0) w.step(1./30.,1,1);
      else if(mode==1) w.step_local(1./30.,1,1,0);
      else w.step_local_multi(1./30.,1,1,0);
    }
    catch(const std::runtime_error &) { threw=true; }
    CHECK(threw);
    CHECK(w.particles()[0].position.x == 0.0);
    CHECK(w.particles()[0].velocity.x == 0.0);
    w.cancel_stroke();
  }
  // Roll back ONLY a failed dab while retaining previously accepted edits;
  // a subsequent valid dab must work and whole-stroke cancel must still work.
  for (int mode=0; mode<3; ++mode) {
    World w;
    w.add_particle({0,0,0},0);
    w.add_particle({1,0,0});
    const auto bi=w.add_bond(0,1,0,.001,25,1.e300);
    w.begin_stroke();
    CHECK(w.apply_grab({1,0,0},.2,{.1,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,1,1,1);
    const double pre_x=w.particles()[1].position.x;
    const Bond pre_bond=w.bonds()[bi];
    CHECK(w.apply_grab({pre_x,0,0},.5,{1.e308,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool threw=false;
    try {
      if(mode==0) w.step(1./30.,1,1);
      else if(mode==1) w.step_local(1./30.,1,1,1);
      else w.step_local_multi(1./30.,1,1,1);
    }
    catch(const std::runtime_error &) { threw=true; }
    CHECK(threw);
    CHECK(w.particles()[1].position.x == pre_x);
    CHECK(w.bonds()[bi].rest_length == pre_bond.rest_length);
    CHECK(w.bonds()[bi].broken == pre_bond.broken);
    CHECK(w.apply_grab({pre_x,0,0},.5,{.01,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,1,1,1);
    w.cancel_stroke();
    CHECK(w.particles()[1].position.x == 1.0);
    CHECK(w.bonds()[bi].rest_length == 1.0);
  }
  // Tiny but finite dt causes h*h to underflow and rigid constraints
  // to compute 0/0. Never accept NaNs for pinned particles.
  {
    World w;
    w.add_particle({0,0,0},0);
    w.add_particle({1,0,0},0);
    w.add_bond(0,1,0);
    w.begin_stroke();
    bool rejected=false;
    try { w.step(1.e-200,1,1); }
    catch (const std::invalid_argument &) { rejected=true; }
    CHECK(rejected);
    CHECK(w.particles()[0].position.x == 0.0);
    CHECK(w.particles()[1].position.x == 1.0);
    CHECK(w.bonds()[0].rest_length == 1.0);
    w.cancel_stroke();
  }
  // Local solver must also reject substeps with underflowing h*h.
  {
    World w;
    w.add_particle({0,0,0});
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1,{.1,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool rejected=false;
    try { w.step_local(1.e-200,1,1,0); }
    catch (const std::invalid_argument &) { rejected=true; }
    CHECK(rejected);
    CHECK(w.particles()[0].position.x == 0.0);
    w.cancel_stroke();
  }
  std::cout << "Numeric step failure is atomic across global/local/multi-local\n";
}
