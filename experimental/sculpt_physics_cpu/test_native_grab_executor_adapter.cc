/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_grab_executor_adapter.hh"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;

int main()
{
  World world;
  std::vector<Vec3> editor;
  for (int i=0;i<15;++i) {
    const Vec3 p{double(i),0,0};
    world.add_particle(p,i==11 ? 0.0 : 1.0); // hidden/masked vertex
    editor.push_back(p);
    if (i) world.add_bond(i-1,i,1.0e-5);
  }
  GpuTopologyBondIndex index(world,19);
  auto position=[&](std::size_t id) -> Vec3 {return editor.at(id);};
  const NativeGrabDab settings{{0,0.30,0},1.0e-6,1.0/60.0,2,6,19};

  auto first=prepare_cpu_native_grab_dab(world,index,
                                        {{5,1.0},{6,0.5}},position,settings);
  assert(first.has_solver());
  assert(!first.writes().empty());
  assert(first.revision()>0);
  for (const auto &change:first.writes()) {
    assert((change.global_index==5 || change.global_index==6));
    assert(change.position.y>0);
    editor[change.global_index]=change.position; // simulate native editor Undo write
  }
  const double first_y=editor[5].y;
  assert(first_y>0 && first_y<=0.30);
  first.finish_editor_success();
  assert(!first.has_solver());
  assert(world.particles()[5].position.y==0.0); // rest World untouched

  // A second Blender dab starts from CURRENT editor positions, not the rest
  // World positions. Moving to another PBVH node does not require expanding
  // or mutating the prior island's lifetime.
  const NativeGrabDab follow{{0,0.1,0},1.0e-6,1.0/60.0,2,6,19};
  auto second=prepare_cpu_native_grab_dab(world,index,
                                          {{6,1.0},{7,0.25}},position,follow);
  assert(!second.writes().empty());
  for (const auto &change:second.writes()) {
    assert(change.global_index==6 || change.global_index==7);
    editor[change.global_index]=change.position;
  }
  second.finish_editor_success();
  assert(editor[5].y==first_y); // earlier native write persists

  // All staging occurs before editor writes; dropping a prepared dab does
  // not mutate mesh or source World (strong pre-write failure isolation).
  const auto snapshot=editor;
  {
    auto canceled=prepare_cpu_native_grab_dab(
        world,index,{{8,1.0}},position,settings);
    assert(canceled.has_solver());
    // Simulate a native PBVH/Undo validation failure: discard staging.
  }
  for (std::size_t i=0;i<editor.size();++i) {
    assert(editor[i].x==snapshot[i].x && editor[i].y==snapshot[i].y);
  }

  // Empty or zero-influence PBVH region is a true no-op.
  auto empty=prepare_cpu_native_grab_dab(
      world,index,{{5,0.0}},position,settings);
  assert(!empty.has_solver() && empty.writes().empty());
  // Hidden particle may still be selected; it must remain fixed.
  auto fixed=prepare_cpu_native_grab_dab(
      world,index,{{11,1.0}},position,settings);
  assert(!fixed.has_solver() && fixed.writes().empty());

  // A stationary cursor must not relax old rest constraints, read the
  // editor positions, or allocate a local solver even on a deformed mesh.
  std::size_t reads=0;
  auto counted_position=[&](std::size_t id)->Vec3 {++reads; return editor.at(id);};
  NativeGrabDab stationary=settings;
  stationary.delta={0.0,0.0,0.0};
  auto still=prepare_cpu_native_grab_dab(
      world,index,{{5,1.0}},counted_position,stationary);
  assert(!still.has_solver() && still.writes().empty() && reads==0);
  auto masked=prepare_cpu_native_grab_dab(
      world,index,{{11,1.0}},counted_position,settings);
  assert(!masked.has_solver() && masked.writes().empty() && reads==0);
  // A selected but distant pinned particle must not enlarge the local
  // physical island or force reading its unrelated neighborhood.
  auto mixed=prepare_cpu_native_grab_dab(
      world,index,{{5,1.0},{11,1.0}},counted_position,settings);
  assert(mixed.has_solver() && reads==3); // vertex 5 and one-ring 4,6
  for (const auto &change : mixed.writes()) assert(change.global_index==5);
  mixed.cancel();

  auto rejects=[&](const std::vector<NativeGrabSample> &samples,
                   const NativeGrabDab &config) {
    bool rejected=false;
    try {(void)prepare_cpu_native_grab_dab(
        world,index,samples,position,config);}
    catch (const std::exception &) {rejected=true;}
    assert(rejected);
  };
  rejects({{5,1.0},{5,0.5}},settings);
  rejects({{99,1.0}},settings);
  rejects({{7,std::numeric_limits<double>::quiet_NaN()}},settings);
  rejects({{7,-0.1}},settings);
  rejects({{7,1.01}},settings);
  NativeGrabDab stale=settings; stale.topology_generation=20;
  rejects({{7,1.0}},stale);
  stationary.topology_generation=20;
  rejects({{7,1.0}},stationary);
  stale=settings; stale.dt=0.0;
  rejects({{7,1.0}},stale);
  stale=settings; stale.compliance=-1.0;
  rejects({{7,1.0}},stale);

  bool rejected=false;
  try {
    auto bad_position=[](std::size_t)->Vec3 {
      return {std::numeric_limits<double>::quiet_NaN(),0,0};
    };
    (void)prepare_cpu_native_grab_dab(world,index,{{5,1.0}},
                                      bad_position,settings);
  }
  catch (const std::invalid_argument &) {rejected=true;}
  assert(rejected);
  return 0;
}
