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

  // A strict island may end at a PINNED halo boundary. Crossing bonds
  // attached solely to such fixed halo nodes have zero positional effect
  // on the editable interior. Thus the entire connected component does
  // not have to be sent to CUDA for a one-ring local XPBD solve.
  World boundary;
  for (int i=0;i<8;++i) boundary.add_particle({double(i),0.0,0.0});
  for (int i=1;i<8;++i) boundary.add_bond(i-1,i,1e-6);
  const GpuTopologyBondIndex boundary_index(boundary,82);
  const auto strict=boundary_index.build(boundary,{3},{2,4},{3},82,false);
  assert(strict.global_ids.size()==3);
  assert(strict.crossing_bonds==0);
  assert(strict.ignored_fixed_crossings==2);
  assert(strict.bonds.size()==2);
  assert(strict.inverse_masses[0]==0 && strict.inverse_masses[2]==0);
  auto reference=build_gpu_local_island(boundary,{3},{2,4},{3},82,false);
  assert(reference.ignored_fixed_crossings==strict.ignored_fixed_crossings);
  assert(reference.bonds.size()==strict.bonds.size());
  auto moved_boundary=strict.positions;
  moved_boundary[1].y=0.25;
  assert(stage_gpu_local_writes(strict,moved_boundary).size()==1);
  moved_boundary[0].y=0.25;
  rejected=false;
  try {(void)stage_gpu_local_writes(strict,moved_boundary);}
  catch (const std::logic_error &) {rejected=true;}
  assert(rejected);
  // By contrast, omitting a directly connected vertex from a FREE active
  // region is unsafe and MUST be rejected in the strict path.
  rejected=false;
  try {(void)boundary_index.build(boundary,{3},{2},{3},82,false);}
  catch (const std::logic_error &) {rejected=true;}
  assert(rejected);

  // Float32 absolute coordinates at 1e12 cannot represent a 2mm change.
  // The GPU workset must pack origin-relative values and reconstruct only
  // genuine edits, without flagging float32 roundoff on pinned halo vertices.
  World far_world;
  far_world.add_particle({1.0e12,0,0});
  far_world.add_particle({1.0e12+0.002,0,0});
  far_world.add_particle({1.0e12+1.0,0,0});
  far_world.add_bond(0,1);
  far_world.add_bond(1,2);
  const auto far_island=build_gpu_local_island(
      far_world,{1},{0,2},{1},77,false);
  const auto packed=pack_gpu_island_relative(far_island);
  assert(packed.relative_positions.size()==3);
  assert(std::abs(static_cast<double>(packed.relative_positions[1].x) -
                  static_cast<double>(packed.relative_positions[0].x) -
                  0.002) < 1.0e-4);
  assert(static_cast<float>(far_world.particles()[0].position.x) ==
         static_cast<float>(far_world.particles()[1].position.x));
  auto local_positions=packed.relative_positions;
  assert(stage_gpu_relative_writes(far_island,packed,local_positions).empty());
  local_positions[1].y += 0.02f;
  const auto far_changes=stage_gpu_relative_writes(
      far_island,packed,local_positions);
  assert(far_changes.size()==1 && far_changes[0].global_index==1);
  assert(std::abs(far_changes[0].position.y-0.02)<1.0e-6);
  assert(far_world.particles()[1].position.y==0.0);
  local_positions[0].x += 0.1f;
  rejected=false;
  try { (void)stage_gpu_relative_writes(far_island,packed,local_positions); }
  catch (const std::logic_error &) { rejected=true; }
  assert(rejected);
  local_positions[0]=packed.relative_positions[0];
  local_positions[1].x=std::numeric_limits<float>::quiet_NaN();
  rejected=false;
  try { (void)stage_gpu_relative_writes(far_island,packed,local_positions); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  return 0;
}
