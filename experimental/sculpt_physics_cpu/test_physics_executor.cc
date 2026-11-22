/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_executor.hh"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;

namespace {
bool near(double a,double b,double tolerance=1e-8)
{
  return std::abs(a-b)<tolerance;
}

GpuLocalIsland sample_island(World &world)
{
  return build_gpu_local_island(world,{1,2},{0},{1,2},77,false);
}

void test_cpu_factory_and_lifetime()
{
  auto exec=make_native_executor(SolverDevice::CPU);
  const auto cap=exec->capabilities();
  assert(cap.device==SolverDevice::CPU && cap.has_real_executor);
  assert(cap.family==SolverFamily::XPBDSurface && cap.supports_compliant_targets);
  bool rejected=false;
  try { (void)make_native_executor(SolverDevice::WarpCUDA); }
  catch(const std::logic_error &) { rejected=true; }
  assert(rejected);
  rejected=false;
  try { (void)make_native_executor(SolverDevice::VulkanCompute); }
  catch(const std::logic_error &) { rejected=true; }
  assert(rejected);

  // A policy may select a device that exists but NOT implement the
  // requested physical solver. Never silently execute surface XPBD for MPM
  // or DEM just because CPU is present.
  SolverPlan supported;
  supported.family=SolverFamily::XPBDSurface;
  supported.device=SolverDevice::CPU;
  supported.implementation_available=true;
  assert(make_native_executor(supported)->capabilities().has_real_executor);
  for (SolverFamily family : {SolverFamily::XPBDVolume,
                              SolverFamily::DEMGrains,
                              SolverFamily::MPMContinuum}) {
    SolverPlan unsupported=supported;
    unsupported.family=family;
    rejected=false;
    try { (void)make_native_executor(unsupported); }
    catch (const std::logic_error &) { rejected=true; }
    assert(rejected);
  }
  SolverPlan unavailable=supported;
  unavailable.implementation_available=false;
  rejected=false;
  try { (void)make_native_executor(unavailable); }
  catch (const std::logic_error &) { rejected=true; }
  assert(rejected);
  unavailable=supported;
  unavailable.device=SolverDevice::WarpCUDA;
  rejected=false;
  try { (void)make_native_executor(unavailable); }
  catch (const std::logic_error &) { rejected=true; }
  assert(rejected);

  World world;
  world.add_particle({0,0,0});
  world.add_particle({1,0,0});
  world.add_particle({2,0,0});
  world.add_bond(0,1);
  world.add_bond(1,2);
  const auto original=sample_island(world);
  exec->allocate_island(original);
  assert(exec->topology_generation()==77);
  assert(!exec->stroke_active());
  exec->begin_stroke();
  assert(exec->stroke_active());
  rejected=false;
  try { exec->allocate_island(original); }
  catch (const std::logic_error &) { rejected=true; }
  assert(rejected);

  // All input checks are atomic, using global IDs instead of GPU-local IDs.
  exec->upload_dirty({{1,{1,0.1,0}}});
  const auto before=exec->download_dirty();
  assert(before.size()==1 && before[0].global_index==1);
  assert(near(before[0].position.y,0.1));
  rejected=false;
  try { exec->upload_dirty({{2,{2,0.15,0}},
                            {0,{0,0.8,0}}}); }
  catch (const std::logic_error &) { rejected=true; }
  assert(rejected && exec->download_dirty().size()==1);
  rejected=false;
  try { exec->upload_dirty({{1,{1,0.2,0}},
                            {1,{1,0.3,0}}}); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected && exec->download_dirty().size()==1);

  exec->set_targets({{2,{2,0.5,0},0.0}});
  rejected=false;
  try { exec->set_targets({{1,{1,0.2,0},0},
                           {1,{1,0.2,0},0}}); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected);

  rejected=false;
  try { exec->step(0.0,1,2); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected && exec->download_dirty().size()==1);
  exec->step(1.0/60.0,1,8);
  const auto moved=exec->download_dirty();
  assert(!moved.empty());
  for (const auto &entry:moved) {
    assert(entry.global_index==1 || entry.global_index==2);
    assert(std::isfinite(entry.position.y));
  }
  // The authoritative World is untouched until the future Blender adapter
  // validates and applies all native PBVH/CCG/Undo modifications.
  assert(near(world.particles()[1].position.y,0));
  assert(near(world.particles()[2].position.y,0));
  exec->cancel();
  assert(!exec->stroke_active());
  exec->begin_stroke();
  assert(exec->download_dirty().empty());

  // An empty local target set is a genuine quasistatic no-op.
  exec->set_targets({});
  exec->step(1.0/60.0,1,5);
  assert(exec->download_dirty().empty());
  exec->upload_dirty({{2,{2,0.3,0}}});
  exec->commit();
  assert(!exec->stroke_active());
  exec->begin_stroke();
  assert(exec->download_dirty().empty()); // committed proxy is the new baseline
  exec->upload_dirty({{2,{2,0.8,0}}});
  assert(exec->download_dirty().size()==1);
  exec->cancel();
  exec->begin_stroke();
  assert(exec->download_dirty().empty()); // cancellation restored 0.3 baseline
  exec->upload_dirty({{2,{2,0.0,0}}});
  assert(exec->download_dirty().size()==1);
  exec->cancel();

  GpuLocalIsland incomplete=original;
  incomplete.crossing_bonds=1;
  rejected=false;
  try { exec->allocate_island(incomplete); }
  catch(const std::logic_error &) { rejected=true; }
  assert(rejected);
  GpuLocalIsland wrong=original;
  wrong.global_ids[1]=wrong.global_ids[0];
  rejected=false;
  try { exec->allocate_island(wrong); }
  catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);

  // A failed reallocation retains the previously verified state.
  exec->begin_stroke();
  assert(exec->topology_generation()==77);
  exec->cancel();
}

void test_compliant_targets_match_native_world()
{
  // Comparing the simplest exact case avoids hiding differences from
  // velocity integration / fracture absent from the standalone executor.
  for (const double compliance: {0.0,1e-5,0.01}) {
    World world;
    world.add_particle({1,0,0});
    GpuLocalIsland island=build_gpu_local_island(world,{0},{},{0},12,false);
    auto exec=make_cpu_xpbd_executor();
    exec->allocate_island(island);
    exec->begin_stroke();
    exec->set_targets({{0,{1.5,0,0},compliance}});
    exec->step(1.0/60.0,1,4);
    const auto edits=exec->download_dirty(1e-14);
    assert(edits.size()==1);

    world.begin_stroke();
    assert(world.set_brush_targets({{0,{1.5,0,0},compliance}}));
    world.step_local(1.0/60.0,1,4,0);
    assert(near(edits[0].position.x,world.particles()[0].position.x,1e-9));
    assert(near(edits[0].position.y,world.particles()[0].position.y,1e-9));
    world.cancel_stroke();
    exec->cancel();
  }
}

void test_fixed_halo_matches_world_bond_projection()
{
  // Compare one real pinned-boundary distance bond against the existing
  // World local XPBD equations (not an abstract/single-vertex toy case).
  for (double compliance : {0.0,1e-5}) {
    World world;
    world.add_particle({0,0,0},0.0); // fixed exterior of PBVH/Undo region
    world.add_particle({1,0,0});
    world.add_bond(0,1,compliance);
    auto exec=make_cpu_xpbd_executor();
    exec->allocate_island(build_gpu_local_island(
        world,{1},{0},{1},33,false));
    exec->begin_stroke();
    exec->set_targets({{1,{1.0,0.4,0.0},1e-6}});
    exec->step(1.0/60.0,1,8);
    auto staged=exec->download_dirty();
    assert(staged.size()==1 && staged[0].global_index==1);

    world.begin_stroke();
    world.set_brush_targets({{1,{1.0,0.4,0.0},1e-6}});
    world.step_local(1.0/60.0,1,8,0);
    assert(near(staged[0].position.x,world.particles()[1].position.x,1e-8));
    assert(near(staged[0].position.y,world.particles()[1].position.y,1e-8));
    assert(near(world.particles()[0].position.y,0.0));
    exec->cancel();
    world.cancel_stroke();
  }
}

void test_incremental_editor_ack_protocol()
{
  World world;
  world.add_particle({0,0,0},0.0);
  world.add_particle({1,0,0});
  world.add_particle({2,0,0});
  world.add_bond(0,1);
  world.add_bond(1,2);

  auto exec=make_cpu_xpbd_executor();
  exec->allocate_island(build_gpu_local_island(world,{1,2},{0},{1,2},99,false));
  exec->begin_stroke();
  assert(exec->stage_incremental().writes.empty());
  exec->upload_dirty({{1,{1,0.1,0}}});
  const auto first=exec->stage_incremental();
  assert(first.writes.size()==1 && first.writes[0].global_index==1);
  assert(first.revision!=0);
  exec->acknowledge_incremental(first.revision);
  assert(exec->stage_incremental().writes.empty());
  // Full-stroke history remains available even after per-dab acknowledgment.
  assert(exec->download_dirty().size()==1);

  exec->set_targets({{2,{2,0.5,0},1e-6}});
  exec->step(1.0/60.0,1,6);
  const auto second=exec->stage_incremental();
  assert(second.revision>first.revision && !second.writes.empty());
  bool rejected=false;
  try {exec->acknowledge_incremental(first.revision);}
  catch(const std::logic_error &) {rejected=true;}
  assert(rejected && exec->stage_incremental().writes.size()==second.writes.size());
  exec->acknowledge_incremental(second.revision);
  assert(exec->stage_incremental().writes.empty());
  assert(!exec->download_dirty().empty());

  // An invalid input cannot advance the revision or falsely acknowledge.
  rejected=false;
  try {exec->upload_dirty({{0,{0,1,0}}});}
  catch(const std::logic_error &) {rejected=true;}
  assert(rejected);
  assert(exec->stage_incremental().revision==second.revision);
  rejected=false;
  try {(void)exec->stage_incremental(-1.0);}
  catch(const std::invalid_argument &) {rejected=true;}
  assert(rejected);

  exec->cancel();
  exec->begin_stroke();
  assert(exec->download_dirty().empty());
  assert(exec->stage_incremental().writes.empty());
  exec->upload_dirty({{2,{2,0.3,0}}});
  const auto third=exec->stage_incremental();
  exec->acknowledge_incremental(third.revision);
  exec->commit();
  exec->begin_stroke();
  assert(exec->stage_incremental().writes.empty());
  assert(exec->download_dirty().empty());
  exec->cancel();
}

void test_repeated_dabs_with_reused_solver_scratch()
{
  World world;
  world.add_particle({0,0,0},0.0);
  for (std::size_t i=1;i<45;++i) {
    world.add_particle({static_cast<double>(i),0.0,0.0});
    world.add_bond(i-1,i,1e-6);
  }
  std::vector<std::size_t> active;
  for (std::size_t i=1;i<45;++i) active.push_back(i);
  auto exec=make_cpu_xpbd_executor();
  exec->allocate_island(build_gpu_local_island(world,active,{0},active,8,false));
  exec->begin_stroke();
  for (int dab=0;dab<100;++dab) {
    const double displacement=(dab%2==0) ? 0.2 : -0.1;
    // Change target count and locations to exercise scratch-capacity reuse.
    if (dab%3==0) {
      exec->set_targets({{10,{10,displacement,0},1e-5},
                         {20,{20,displacement*0.5,0},1e-5}});
    }
    else {
      exec->set_targets({{10,{10,displacement,0},1e-5}});
    }
    exec->step(1.0/60.0,2,3);
    auto batch=exec->stage_incremental();
    for (const auto &e : batch.writes) {
      assert(e.global_index!=0 && std::isfinite(e.position.x) &&
             std::isfinite(e.position.y));
    }
    exec->acknowledge_incremental(batch.revision);
    assert(exec->stage_incremental().writes.empty());
  }
  assert(!exec->download_dirty().empty());
  exec->cancel();
  exec->begin_stroke();
  assert(exec->download_dirty().empty());
  exec->cancel();
}

void test_sparse_ack_on_large_proxy()
{
  World world;
  std::vector<std::size_t> all;
  all.reserve(5000);
  for (std::size_t i=0;i<5000;++i) {
    world.add_particle({static_cast<double>(i),0,0});
    all.push_back(i);
  }
  // All 5000 vertices are in the proxy, but only two change on this dab.
  // No bonds: isolate sparse acknowledgment from physics propagation.
  auto exec=make_cpu_xpbd_executor();
  exec->allocate_island(build_gpu_local_island(world,all,{},all,52,false));
  exec->begin_stroke();
  exec->upload_dirty({{4000,{4000,0.3,0}},{10,{10,0.1,0}}});
  auto first=exec->stage_incremental();
  assert(first.writes.size()==2);
  assert(first.writes[0].global_index==10 &&
         first.writes[1].global_index==4000);
  exec->acknowledge_incremental(first.revision);
  assert(exec->stage_incremental().writes.empty());
  exec->upload_dirty({{300,{300,0.2,0}}});
  auto second=exec->stage_incremental();
  assert(second.writes.size()==1 && second.writes[0].global_index==300);
  exec->acknowledge_incremental(second.revision);
  assert(exec->stage_incremental().writes.empty());
  assert(exec->download_dirty().size()==3);
  exec->cancel();
  exec->begin_stroke();
  assert(exec->download_dirty().empty());
  exec->cancel();
}

void test_target_strength_and_atomic_step()
{
  World world;
  world.add_particle({0,0,0});
  auto exec=make_cpu_xpbd_executor();
  exec->allocate_island(build_gpu_local_island(world,{0},{},{0},1,false));
  exec->begin_stroke();
  exec->set_targets({{0,{0,1,0},0.01}});
  exec->step(1.0/60.0,1,4);
  const double soft=exec->download_dirty()[0].position.y;
  assert(soft>0 && soft<0.1);
  exec->cancel();
  exec->begin_stroke();
  exec->set_targets({{0,{0,1,0},0.0}});
  exec->step(1.0/60.0,1,4);
  assert(near(exec->download_dirty()[0].position.y,1.0));
  // A rejected timestep must not damage the previously solved position.
  bool rejected=false;
  try { exec->step(1e-300,1,4); }
  catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  assert(near(exec->download_dirty()[0].position.y,1.0));
  exec->cancel();
  exec->begin_stroke();
  assert(exec->download_dirty().empty());
  exec->cancel();
}
} // namespace

int main()
{
  test_cpu_factory_and_lifetime();
  test_compliant_targets_match_native_world();
  test_fixed_halo_matches_world_bond_projection();
  test_target_strength_and_atomic_step();
  test_incremental_editor_ack_protocol();
  test_repeated_dabs_with_reused_solver_scratch();
  test_sparse_ack_on_large_proxy();
  return 0;
}
