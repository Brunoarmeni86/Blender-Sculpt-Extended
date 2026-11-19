/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace sculpt_physics;
#define CHECK(c) do { if(!(c)) throw std::runtime_error("FAILED: " #c); } while(false)
static bool near(double a, double b, double eps=1e-8) { return std::abs(a-b)<eps; }

static World make_two_components(double compliance)
{
  World w;
  for (int i=0;i<3;++i) w.add_particle({double(i),0,0});
  for (int i=0;i<3;++i) w.add_particle({double(i+100),0,0});
  for (int i=0;i<2;++i) {
    w.add_bond(i,i+1,compliance);
    w.add_bond(i+3,i+4,compliance);
  }
  return w;
}
static void same_physical_state(const World &a, const World &b)
{
  CHECK(a.particles().size()==b.particles().size());
  CHECK(a.bonds().size()==b.bonds().size());
  for (size_t i=0;i<a.particles().size();++i) {
    const Particle &p=a.particles()[i], &q=b.particles()[i];
    CHECK(std::isfinite(p.position.x) && std::isfinite(p.position.y));
    CHECK(near(p.position.x,q.position.x));
    CHECK(near(p.position.y,q.position.y));
    CHECK(near(p.velocity.x,q.velocity.x));
    CHECK(near(p.velocity.y,q.velocity.y));
  }
  for (size_t i=0;i<a.bonds().size();++i) {
    CHECK(near(a.bonds()[i].rest_length,b.bonds()[i].rest_length));
    CHECK(a.bonds()[i].broken==b.bonds()[i].broken);
  }
}
int main()
{
  // 4 stiffness regimes x 160 alternating strokes, with repeated
  // commit/cancel. Both complete bonded components are in the active union.
  for (const double compliance : {0.0,1e-10,1e-5,1e-2}) {
    World global=make_two_components(compliance);
    World multi=make_two_components(compliance);
    for (int dab=0; dab<160; ++dab) {
      const World old_global=global, old_multi=multi;
      global.begin_stroke();
      multi.begin_stroke();
      const int idx=(dab%2==0)?0:3;
      const Vec3 center=global.particles()[idx].position;
      const double shift=(dab%5==0)?0.05:-0.025;
      const BrushPhysicsSettings brush{BrushPhysicsMode::FullPhysics,1,1e-7};
      CHECK(global.apply_grab(center,.3,{0,shift,0},brush));
      CHECK(multi.apply_grab(center,.3,{0,shift,0},brush));
      global.step(1./60.,2,6);
      multi.step_local_multi(1./60.,2,6,3);
      same_physical_state(global,multi);
      if (dab%4==0) {
        global.cancel_stroke();
        multi.cancel_stroke();
        same_physical_state(global,old_global);
        same_physical_state(multi,old_multi);
      }
      else {
        global.commit_stroke();
        multi.commit_stroke();
      }
    }
  }
  // The comparison also covers material memory: plastic rest lengths and
  // breakable bonds, with alternating accepted and cancelled strokes.
  {
    World global, multi;
    for (World *w : {&global,&multi}) {
      for (int i=0;i<3;++i) w->add_particle({double(i),0,0}, i==0?0.0:1.0);
      for (int i=0;i<3;++i) w->add_particle({double(i+100),0,0}, i==0?0.0:1.0);
      for (int i=0;i<2;++i) {
        w->add_bond(i,i+1,1e-5,.01,20.0,.30);
        w->add_bond(i+3,i+4,1e-5,.01,20.0,.30);
      }
    }
    for (int dab=0;dab<80;++dab) {
      const World old_global=global,old_multi=multi;
      global.begin_stroke();multi.begin_stroke();
      const int id=(dab%2==0)?2:5;
      const Vec3 center=global.particles()[id].position;
      const Vec3 delta{0,(dab%4==0)?0.35:-0.08,0};
      const BrushPhysicsSettings brush{BrushPhysicsMode::FullPhysics,1.0,0};
      CHECK(global.apply_grab(center,.3,delta,brush));
      CHECK(multi.apply_grab(center,.3,delta,brush));
      global.step(1./60.,2,6);
      multi.step_local_multi(1./60.,2,6,3);
      same_physical_state(global,multi);
      if(dab%5==0) {
        global.cancel_stroke(); multi.cancel_stroke();
        same_physical_state(global,old_global);
        same_physical_state(multi,old_multi);
      }
      else {global.commit_stroke(); multi.commit_stroke();}
    }
  }
  // Previously cancel restored particles but leaked gravity, damping and
  // floor settings into the following Sculpt brush stroke.
  {
    World w;
    w.add_particle({0,1,0});
    w.begin_stroke();
    w.set_gravity({0,-9.81,0});
    w.set_velocity_damping(17.0);
    w.set_ground_height(10.0);
    w.cancel_stroke();
    w.begin_stroke();
    CHECK(w.apply_grab({0,1,0},.2,{0,.1,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,1,1,0);
    CHECK(!w.last_step_statistics().used_global_fallback);
    CHECK(w.particles()[0].position.y<2.0);
    w.cancel_stroke();
  }
  // Commit of settings establishes the next stroke's baseline.
  {
    World w;
    w.add_particle({0,1,0});
    w.begin_stroke();
    w.set_gravity({0,-9.81,0});
    w.set_velocity_damping(5.0);
    w.set_ground_height(0.0);
    w.commit_stroke();
    w.begin_stroke();
    w.disable_ground();
    w.set_gravity({0,0,0});
    w.set_velocity_damping(0.0);
    w.cancel_stroke();
    w.begin_stroke();
    CHECK(w.apply_grab({0,1,0},.2,{0,.1,0},
                       {BrushPhysicsMode::FullPhysics,1,0}));
    w.step_local(1./30.,1,1,0);
    CHECK(w.last_step_statistics().used_global_fallback);
    w.cancel_stroke();
  }
  std::cout<<"Long-run multistroke and configuration transaction tests passed\n";
}
