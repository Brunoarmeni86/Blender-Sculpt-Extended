/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace sculpt_physics;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("FAILED: " #c); } while(false)

int main()
{
  // A failed first global step must leave the Undo journal sparse and usable.
  {
    World w;
    w.add_particle({0,0,0}); w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1,{1e308,0,0},{BrushPhysicsMode::FullPhysics,1,0}));
    bool rejected=false;
    try { w.step(1./30.,1,1); } catch (const std::runtime_error &) { rejected=true; }
    CHECK(rejected && !w.stroke_journal_statistics().dense_fallback);
    CHECK(w.stroke_journal_statistics().particles_recorded==0);
    CHECK(w.particles()[0].position.x==0);
    CHECK(w.apply_grab({0,0,0},1,{.25,0,0},{BrushPhysicsMode::FullPhysics,1,0}));
    w.step(1./30.,1,1);
    CHECK(w.stroke_journal_statistics().dense_fallback);
    w.cancel_stroke();
    CHECK(w.particles()[0].position.x==0);
  }
  // A local plastic dab, failed global dab and successful global dab must
  // still cancel the entire stroke to its ORIGINAL pre-local state.
  {
    World w;
    w.add_particle({0,0,0},0);
    w.add_particle({1,0,0});
    const auto bi=w.add_bond(0,1,0,.01,20.0,1000.0);
    const Bond original_bond=w.bonds()[bi];
    w.begin_stroke();
    CHECK(w.apply_grab({1,0,0},.2,{.6,0,0},{BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,2,5,1);
    const auto after_local=w.particles()[1];
    const auto after_bond=w.bonds()[bi];
    const auto saved_stats=w.stroke_journal_statistics();
    CHECK(!saved_stats.dense_fallback && saved_stats.particles_recorded>=1);
    CHECK(w.apply_grab({after_local.position.x,0,0},.5,{1e308,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool rejected=false;
    try { w.step(1./30.,1,1); } catch (const std::runtime_error &) { rejected=true; }
    CHECK(rejected);
    const auto now=w.stroke_journal_statistics();
    CHECK(!now.dense_fallback);
    CHECK(now.particles_recorded==saved_stats.particles_recorded);
    CHECK(now.bonds_recorded==saved_stats.bonds_recorded);
    CHECK(w.particles()[1].position.x==after_local.position.x);
    CHECK(w.bonds()[bi].rest_length==after_bond.rest_length);
    CHECK(w.apply_grab({after_local.position.x,0,0},.5,{.01,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    w.step(1./30.,1,1);
    CHECK(w.stroke_journal_statistics().dense_fallback);
    w.cancel_stroke();
    CHECK(w.particles()[1].position.x==1.0);
    CHECK(w.bonds()[bi].rest_length==original_bond.rest_length);
    CHECK(w.bonds()[bi].broken==original_bond.broken);
  }
  // h*h may be positive but compliance/(h*h) can still overflow. Pinned
  // bonds must not silently retain NaN multipliers on a successful call.
  for (int mode=0; mode<2; ++mode) {
    World w;
    w.add_particle({0,0,0},mode==0?0.0:1.0);
    w.add_particle({1,0,0},0.0);
    w.add_bond(0,1,1e100);
    w.begin_stroke();
    if(mode==1) CHECK(w.apply_grab({0,0,0},.25,{.1,0,0},
                                   {BrushPhysicsMode::FullPhysics,1,0}));
    bool rejected=false;
    try {
      if(mode==0)w.step(1e-150,1,1);
      else w.step_local(1e-150,1,1,1);
    } catch (const std::runtime_error &) { rejected=true; }
    CHECK(rejected);
    CHECK(std::isfinite(w.bonds()[0].lambda));
    CHECK(w.bonds()[0].lambda==0);
    CHECK(w.particles()[0].position.x==0.0);
    CHECK(w.particles()[1].position.x==1.0);
    CHECK(!w.stroke_journal_statistics().dense_fallback);
    w.cancel_stroke();
  }
  std::cout<<"V8 deferred Undo and scaled-compliance tests passed\n";
}
