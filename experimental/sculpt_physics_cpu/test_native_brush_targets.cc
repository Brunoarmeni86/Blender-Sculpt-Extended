/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace sculpt_physics;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("FAILED: " #expr); } while(false)

int main()
{
  // Blender has already computed mask/texture/symmetry falloff here. The solver
  // must accept those exact targets without repeating its own radial falloff.
  World world;
  world.add_particle({0,0,0});
  world.add_particle({1,0,0},0.0); // Fully masked/pinned.
  world.add_particle({2,0,0});
  world.add_bond(0,1,1e-5);
  world.add_bond(1,2,1e-5);
  world.begin_stroke();
  CHECK(world.set_brush_targets({{2,{2,.25,0},0}, {1,{1,5,0},0}, {0,{0,.5,0},0}}));
  world.step_local(1.0/60.0,2,5,0);
  CHECK(world.particles()[0].position.y>0);
  CHECK(world.particles()[1].position.y==0);
  CHECK(world.particles()[2].position.y>0);
  const double previous=world.particles()[0].position.y;
  // Both invalid geometry and duplicate candidate IDs must leave old targets
  // intact so the next valid dab can continue.
  bool threw=false;
  try {world.set_brush_targets({{0,{NAN,0,0},0}});}
  catch(const std::invalid_argument &){threw=true;}
  CHECK(threw);
  threw=false;
  try {world.set_brush_targets({{0,{0,1,0},0},{0,{0,2,0},0}});}
  catch(const std::invalid_argument &){threw=true;}
  CHECK(threw);
  world.step_local(1.0/60.0,2,5,0);
  CHECK(world.particles()[0].position.y>=previous);
  // Topology changes while undo is recording must never be permitted.
  threw=false;
  try {world.add_particle({3,0,0});}
  catch(const std::logic_error &){threw=true;}
  CHECK(threw);
  threw=false;
  try {world.add_bond(0,2,1e-5);}
  catch(const std::logic_error &){threw=true;}
  CHECK(threw);
  world.cancel_stroke();
  CHECK(world.particles()[0].position.y==0);
  CHECK(world.particles()[1].position.y==0);
  CHECK(world.particles()[2].position.y==0);
  CHECK(world.bonds().size()==2);

  // Empty native brush constraints must clear prior targets.
  World empty;
  empty.add_particle({0,0,0});
  empty.begin_stroke();
  CHECK(empty.set_brush_targets({{0,{0,.1,0},0}}));
  CHECK(!empty.set_brush_targets({}));
  empty.step_local(1./60.,1,2,0);
  CHECK(empty.particles()[0].position.y==0);
  empty.cancel_stroke();
  std::cout<<"V12 exact native brush target and transactional topology tests passed\n";
}
