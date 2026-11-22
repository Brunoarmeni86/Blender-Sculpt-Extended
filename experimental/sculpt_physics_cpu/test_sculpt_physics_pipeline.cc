/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sculpt_physics_pipeline.hh"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;

static bool near(double a, double b) { return std::abs(a-b) < 1.0e-8; }

static CpuDabInput grab(std::size_t id)
{
  CpuDabInput d;
  d.brush.kind = AssistedBrushKind::Grab;
  d.brush.drag_delta = {0.0, 0.5, 0.0};
  d.brush.compliance = 0.0;
  d.physics = {BrushPhysicsMode::FullPhysics, 1.0, 0.0};
  d.selection = {{id, {}, 1.0}};
  d.undo_covered_vertices = {id};
  return d;
}

static void test_native_mesh_staging_and_rollback()
{
  World world;
  world.add_particle({0,0,0});
  world.add_particle({1,0,0});
  world.add_particle({2,0,0});
  world.add_bond(0,1);
  world.add_bond(1,2);
  CpuStrokePipeline runtime(world);
  SolverRequest request;
  request.material = SculptMaterial::Clay;
  runtime.begin(request);

  auto standard = grab(1);
  standard.physics.mode = BrushPhysicsMode::Standard;
  const auto no_op = runtime.dab(standard);
  assert(!no_op.used_solver && no_op.writes.empty());
  assert(near(world.particles()[1].position.y,0.0));

  auto actual = runtime.dab(grab(1));
  assert(actual.used_solver);
  assert(!actual.writes.empty());
  for (const auto &write : actual.writes) assert(write.particle_index == 1);
  assert(world.particles()[1].position.y > 0.0);
  runtime.cancel();
  assert(!runtime.active() && !world.stroke_active());
  assert(near(world.particles()[1].position.y,0.0));

  runtime.begin(request);
  auto invalid = grab(1);
  invalid.selection[0].factor = std::numeric_limits<double>::quiet_NaN();
  bool failed = false;
  try { (void)runtime.dab(invalid); }
  catch (const std::invalid_argument &) { failed = true; }
  assert(failed && !runtime.active() && !world.stroke_active());
  assert(near(world.particles()[1].position.y,0.0));

  runtime.begin(request);
  actual = runtime.dab(grab(1));
  assert(actual.used_solver);
  auto uncovered = grab(1);
  uncovered.undo_covered_vertices = {0};
  failed = false;
  try { (void)runtime.dab(uncovered); }
  catch (const std::logic_error &) { failed = true; }
  assert(failed && !runtime.active() && !world.stroke_active());
  assert(near(world.particles()[1].position.y,0.0));

  runtime.begin(request);
  (void)runtime.dab(grab(1));
  runtime.commit();
  assert(!runtime.active() && world.particles()[1].position.y > 0.0);
}

static void test_multires_sparse_dirty_only()
{
  World world;
  world.add_particle({0,0,0});
  world.add_particle({1,0,0});
  world.add_particle({2,0,0});
  SurfaceAnchor a;
  a.vertices[0]=0; a.vertices[1]=1; a.vertices[2]=2;
  a.weights[0]=0.5; a.weights[1]=0.5; a.weights[2]=0.0;
  SurfaceAnchor b = a;
  b.weights[0]=0.0; b.weights[1]=0.0; b.weights[2]=1.0;
  CpuStrokePipeline runtime(world);
  runtime.configure_multires({a,b,a},2);
  runtime.begin(SolverRequest{});
  const auto result = runtime.dab(grab(1));
  assert(result.used_solver);
  assert((result.dirty_multires_samples == std::vector<std::size_t>{0,2}));
  assert((result.dirty_multires_tiles == std::vector<std::size_t>{0,1}));
  bool rejected=false;
  try {runtime.clear_multires();}
  catch (const std::logic_error &) {rejected=true;}
  assert(rejected);
  runtime.cancel();
  runtime.clear_multires();
}

static void test_granular_plan_and_backend_fail_closed()
{
  World sand;
  sand.add_particle({0,0,0},1.0,0.5);
  sand.add_particle({0.6,0,0},1.0,0.5);
  CpuStrokePipeline session(sand);
  SolverRequest request;
  request.material = SculptMaterial::Sand;
  session.begin(request);
  CpuDabInput contact;
  contact.brush.kind=AssistedBrushKind::GranularDrag;
  contact.physics={BrushPhysicsMode::FullPhysics,1.0,0.0};
  contact.granular_candidates={0,1};
  contact.undo_covered_vertices={0,1};
  const auto result=session.dab(contact);
  assert(result.used_solver && !result.writes.empty());
  assert(sand.particles()[0].position.x < 0.0);
  assert(sand.particles()[1].position.x > 0.6);
  session.cancel();
  assert(near(sand.particles()[0].position.x,0.0));
  assert(near(sand.particles()[1].position.x,0.6));

  AvailablePhysics available;
  available.warp_cuda_ready=true;
  available.warp_surface_ready=true;
  request.material=SculptMaterial::Clay;
  request.active_proxy_particles=100000;
  request.prefer_gpu=true;
  bool rejected=false;
  try {session.begin(request,available);}
  catch (const std::logic_error &) {rejected=true;}
  assert(rejected && !sand.stroke_active());

  request.prefer_gpu=false;
  request.volume_cage_available=true;
  rejected=false;
  try {session.begin(request,available);}
  catch (const std::logic_error &) {rejected=true;}
  assert(rejected && !sand.stroke_active());
}

static void test_scope_destructor_cancels()
{
  World world;
  world.add_particle({0,0,0});
  {
    CpuStrokePipeline runtime(world);
    runtime.begin(SolverRequest{});
    (void)runtime.dab(grab(0));
    assert(world.particles()[0].position.y > 0.0);
  }
  assert(!world.stroke_active() && near(world.particles()[0].position.y,0.0));
}

int main()
{
  test_native_mesh_staging_and_rollback();
  test_multires_sparse_dirty_only();
  test_granular_plan_and_backend_fail_closed();
  test_scope_destructor_cancels();
  return 0;
}
