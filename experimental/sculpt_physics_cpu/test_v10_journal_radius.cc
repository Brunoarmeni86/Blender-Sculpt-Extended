/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <iostream>
#include <stdexcept>
using namespace sculpt_physics;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("FAILED: " #c); } while (false)
int main()
{
  // The squared radius underflows but a particle at the exact center is hit.
  {
    World w;
    w.add_particle({0,0,0}); w.add_particle({1,0,0});
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1e-200,{.2,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    CHECK(w.stroke_journal_statistics().particles_recorded==0);
    w.cancel_stroke();
  }
  // A numerically failed dab leaves NO new stroke Undo entries.
  {
    World w; w.add_particle({0,0,0}); w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},1,{1e308,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool threw=false;
    try { w.step_local(1./30.,1,1,0); }
    catch (const std::runtime_error &) { threw=true; }
    CHECK(threw);
    CHECK(w.particles()[0].position.x==0);
    CHECK(w.stroke_journal_statistics().particles_recorded==0);
    CHECK(w.stroke_journal_statistics().bonds_recorded==0);
    CHECK(w.apply_grab({0,0,0},1,{.2,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,1,1,0);
    CHECK(w.stroke_journal_statistics().particles_recorded==1);
    w.cancel_stroke(); CHECK(w.particles()[0].position.x==0);
  }
  // Earlier successful edits remain in Undo when a second island fails.
  {
    World w; w.add_particle({0,0,0}); w.add_particle({2,0,0});
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},.3,{.2,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,1,1,0);
    CHECK(w.stroke_journal_statistics().particles_recorded==1);
    const double before=w.particles()[0].position.x;
    CHECK(w.apply_grab({2,0,0},.3,{1e308,0,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool threw=false;
    try { w.step_local_multi(1./30.,1,1,0); }
    catch (const std::runtime_error &) { threw=true; }
    CHECK(threw && w.stroke_journal_statistics().particles_recorded==1);
    CHECK(w.particles()[0].position.x==before);
    w.cancel_stroke();
    CHECK(w.particles()[0].position.x==0);
  }
  // A valid local candidate is rejected by a stiff artificial boundary.
  // The subsequent global fallback fails only on a distant bond. Neither
  // candidate nor fallback may leave ghost Undo records in the stroke.
  {
    World w;
    for (int i=0;i<5;++i) w.add_particle({double(i),0,0});
    for (int i=0;i<3;++i) w.add_bond(i,i+1,0);
    w.add_bond(3,4,1e308); // Finite input, scaled alpha overflows on global step.
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},.2,{0,1,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    bool threw=false;
    try { (void)w.step_local_adaptive(1./30.,1,8,0,0,.02); }
    catch (const std::runtime_error &) { threw=true; }
    CHECK(threw);
    CHECK(w.stroke_journal_statistics().particles_recorded==0);
    CHECK(w.stroke_journal_statistics().bonds_recorded==0);
    CHECK(!w.stroke_journal_statistics().dense_fallback);
    CHECK(w.particles()[0].position.y==0);
    w.cancel_stroke();
  }
  std::cout << "V10 journal and tiny-radius regressions passed\n";
}
