/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu_workset.hh"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;

int main()
{
  World world;
  world.add_particle({0,0,0});
  world.add_particle({1,0,0});
  world.add_particle({2,0,0});
  world.add_bond(0,1);
  world.add_bond(1,2);

  const auto island = build_gpu_local_island(
      world,{1},{2,0},{1},42,false);
  assert(island.topology_generation==42);
  assert(island.global_ids.size()==3);
  assert(island.bonds.size()==2 && island.crossing_bonds==0);
  assert(island.global_ids[1]==1);
  assert(island.writable[1]);
  assert(!island.writable[0] && !island.writable[2]);
  assert(island.inverse_masses[0]==0 && island.inverse_masses[2]==0);

  auto solved=island.positions;
  solved[1].y=0.2;
  const auto staged=stage_gpu_local_writes(island,solved);
  assert(staged.size()==1 && staged[0].global_index==1);
  assert(std::abs(staged[0].position.y-0.2)<1.0e-9);
  assert(std::abs(world.particles()[1].position.y)<1.0e-9);

  // A GPU kernel must never change a pinned outside-halo vertex.
  solved[0].y=0.1;
  bool rejected=false;
  try { (void)stage_gpu_local_writes(island,solved); }
  catch(const std::logic_error &) { rejected=true; }
  assert(rejected);
  solved[0].y=0.0;
  solved[1].y=std::numeric_limits<double>::quiet_NaN();
  rejected=false;
  try { (void)stage_gpu_local_writes(island,solved); }
  catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);

  rejected=false;
  try { (void)build_gpu_local_island(world,{1},{0},{1},42,false); }
  catch(const std::logic_error &) { rejected=true; }
  assert(rejected);
  auto cut=build_gpu_local_island(world,{1},{0},{1},42,true);
  assert(cut.crossing_bonds==1 && cut.bonds.size()==1);

  rejected=false;
  try { (void)build_gpu_local_island(world,{1},{0,2},{0},42,false); }
  catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);

  rejected=false;
  try { (void)build_gpu_local_island(world,{99},{0,2},{99},42,false); }
  catch(const std::out_of_range &) { rejected=true; }
  assert(rejected);
  return 0;
}
