/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("FAILED: " #c); } while(false)
static bool eq(double a, double b) { return a == b; }
static void matches(const World &w, const std::vector<Particle> &particles,
                    const std::vector<Bond> &bonds)
{
  CHECK(w.particles().size() == particles.size());
  CHECK(w.bonds().size() == bonds.size());
  for (std::size_t i=0;i<particles.size();++i) {
    const auto &a=w.particles()[i], &b=particles[i];
    CHECK(eq(a.position.x,b.position.x) && eq(a.position.y,b.position.y) && eq(a.position.z,b.position.z));
    CHECK(eq(a.velocity.x,b.velocity.x) && eq(a.velocity.y,b.velocity.y) && eq(a.velocity.z,b.velocity.z));
    CHECK(eq(a.inverse_mass,b.inverse_mass) && eq(a.radius,b.radius));
  }
  for (std::size_t i=0;i<bonds.size();++i) {
    const auto &a=w.bonds()[i], &b=bonds[i];
    CHECK(a.a==b.a && a.b==b.b && a.rest_length==b.rest_length &&
          a.compliance==b.compliance && a.lambda==b.lambda && a.broken==b.broken &&
          a.yield_strain==b.yield_strain && a.plastic_creep==b.plastic_creep &&
          a.fracture_strain==b.fracture_strain);
  }
}
int main()
{
  // Empty strokes do not allocate a dense physical-state snapshot.
  {
    World w;
    for(int i=0;i<20000;++i)w.add_particle({double(i),0,0});
    w.add_bond(0,1,1.e-5);
    const auto original=w.particles(); const auto original_bonds=w.bonds();
    w.begin_stroke();
    auto s=w.stroke_journal_statistics();
    CHECK(s.particles_recorded==0 && s.bonds_recorded==0 && !s.dense_fallback);
    CHECK(!w.apply_grab({999999,0,0},.2,{0,1,0},{BrushPhysicsMode::PhysicalAssist,1,0}));
    w.step_local_multi(1./30.,2,5,3);
    s=w.stroke_journal_statistics();
    CHECK(s.particles_recorded==0 && s.bonds_recorded==0);
    w.commit_stroke();
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},.2,{0,.2,0},{BrushPhysicsMode::PhysicalAssist,1,0}));
    w.step_local_multi(1./30.,2,5,2);
    s=w.stroke_journal_statistics();
    CHECK(s.particles_recorded>=1 && s.particles_recorded<=2);
    CHECK(s.bonds_recorded==1 && !s.dense_fallback);
    CHECK(s.approximate_payload_bytes < 1024);
    w.step_local_multi(1./30.,2,5,2);
    CHECK(w.stroke_journal_statistics().particles_recorded==s.particles_recorded);
    CHECK(w.stroke_journal_statistics().bonds_recorded==s.bonds_recorded);
    w.cancel_stroke();
    matches(w,original,original_bonds);
    CHECK(w.stroke_journal_statistics().particles_recorded==0);
  }
  // A local plastic dab followed by global fallback must undo to original.
  {
    World w;
    w.add_particle({0,0,0},0);
    w.add_particle({1,0,0});
    w.add_particle({2,0,0});
    w.add_bond(0,1,0,.01,20.0,1000.0);
    w.add_bond(1,2,0);
    const auto p=w.particles(); const auto b=w.bonds();
    w.begin_stroke();
    CHECK(w.apply_grab({1,0,0},.2,{.6,0,0},{BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,2,5,1);
    CHECK(w.bonds()[0].rest_length > b[0].rest_length);
    CHECK(!w.stroke_journal_statistics().dense_fallback);
    w.set_gravity({0,-9.81,0});
    w.step_local(1./30.,2,5,1);
    CHECK(w.last_step_statistics().used_global_fallback);
    const auto s=w.stroke_journal_statistics();
    CHECK(s.dense_fallback);
    CHECK(s.particles_recorded==w.particles().size());
    CHECK(s.bonds_recorded==w.bonds().size());
    w.cancel_stroke();
    matches(w,p,b);
  }
  // Fracture + distant dabs must preserve the first recorded bond state.
  {
    World w;
    w.add_particle({0,0,0},0);
    w.add_particle({1,0,0});
    w.add_particle({10,0,0});
    w.add_bond(0,1,0,INFINITY,0,.05);
    const auto p=w.particles(); const auto b=w.bonds();
    w.begin_stroke();
    CHECK(w.apply_grab({1,0,0},.2,{1,0,0},{BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local_multi(.1,1,4,1);
    CHECK(w.bonds()[0].broken);
    CHECK(!w.stroke_journal_statistics().dense_fallback);
    CHECK(w.apply_grab({10,0,0},.2,{0,1,0},{BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local_multi(.1,1,4,1);
    CHECK(w.stroke_journal_statistics().particles_recorded<=2);
    w.cancel_stroke();
    matches(w,p,b);
  }
  // Commit drops history; subsequent strokes have independent first-touch sets.
  {
    World w;
    w.add_particle({0,0,0});
    w.begin_stroke();
    w.apply_grab({0,0,0},.2,{0,.2,0},{BrushPhysicsMode::FullPhysics,1,0});
    w.step_local(.05,1,1,0);
    CHECK(w.stroke_journal_statistics().particles_recorded==1);
    w.commit_stroke();
    CHECK(w.stroke_journal_statistics().particles_recorded==0);
    const auto committed=w.particles();
    w.begin_stroke();
    CHECK(w.stroke_journal_statistics().particles_recorded==0);
    w.apply_grab(committed[0].position,.2,{0,.3,0},{BrushPhysicsMode::FullPhysics,1,0});
    w.step_local(.05,1,1,0);
    w.cancel_stroke();
    matches(w,committed,{});
  }
  std::cout<<"Sparse/dense hybrid Undo and persistent local scratch tests passed\n";
}
