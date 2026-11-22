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

  // Indexed local bond lookup must match the full O(B) scanner exactly,
  // including original bond order, broken-bond filtering and cross-boundary
  // policies. A persistent index is created ONCE for many dabs.
  GpuTopologyBondIndex index(world, 42);
  assert(index.particle_count() == 3 && index.bond_count() == 2);
  const auto indexed = index.build(world,{1},{2,0},{1},42,false);
  assert(indexed.global_ids == island.global_ids);
  assert(indexed.inverse_masses == island.inverse_masses);
  assert(indexed.writable == island.writable);
  assert(indexed.bonds.size() == island.bonds.size());
  for (std::size_t i = 0; i < indexed.bonds.size(); ++i) {
    assert(indexed.bonds[i].a == island.bonds[i].a);
    assert(indexed.bonds[i].b == island.bonds[i].b);
    assert(indexed.bonds[i].rest_length == island.bonds[i].rest_length);
  }
  assert(index.build(world,{1},{0},{1},42,true).crossing_bonds == 1);
  rejected = false;
  try { (void)index.build(world,{1},{2,0},{1},43,false); }
  catch(const std::logic_error &) { rejected = true; }
  assert(rejected);
  World other;
  other.add_particle({0,0,0}); other.add_particle({1,0,0}); other.add_particle({2,0,0});
  other.add_bond(0,1); other.add_bond(1,2);
  rejected = false;
  try { (void)index.build(other,{1},{2,0},{1},42,false); }
  catch(const std::logic_error &) { rejected = true; }
  assert(rejected);

  // Larger connected topology and repeated local dabs: a CSR query visits
  // only bonds adjacent to its active/halo IDs, not all distant bonds.
  World large;
  for (std::size_t i = 0; i < 120; ++i) {
    large.add_particle({static_cast<double>(i), 0.0, 0.0});
  }
  for (std::size_t i = 1; i < 120; ++i) large.add_bond(i-1,i);
  GpuTopologyBondIndex fast(large, 7);
  for (std::size_t seed = 5; seed < 110; seed += 7) {
    const std::vector<std::size_t> active{seed};
    const std::vector<std::size_t> halo{seed-2,seed-1,seed+1,seed+2};
    const auto slow = build_gpu_local_island(large,active,halo,active,7,true);
    const auto quick = fast.build(large,active,halo,active,7,true);
    assert(slow.global_ids == quick.global_ids);
    assert(slow.crossing_bonds == quick.crossing_bonds);
    assert(slow.bonds.size() == quick.bonds.size());
    for (std::size_t j = 0; j < slow.bonds.size(); ++j) {
      assert(slow.bonds[j].a == quick.bonds[j].a);
      assert(slow.bonds[j].b == quick.bonds[j].b);
    }
  }
  // Adding a bond invalidates the old graph even if caller passes generation 7.
  large.add_bond(0,119);
  rejected = false;
  try { (void)fast.build(large,{5},{4,6},{5},7,true); }
  catch(const std::logic_error &) { rejected = true; }
  assert(rejected);
  return 0;
}
