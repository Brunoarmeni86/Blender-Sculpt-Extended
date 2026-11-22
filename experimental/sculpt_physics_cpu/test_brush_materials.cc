/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "brush_targets.hh"
#include "granular_contacts.hh"
#include "multires_dirty_map.hh"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;

static bool near(double a, double b)
{
  return std::abs(a - b) < 1.0e-8;
}

static void test_brush_goals()
{
  World world;
  world.add_particle({0.0, 0.0, 0.0});
  world.add_particle({1.0, 1.0, 0.0});
  world.add_particle({2.0, 0.0, 0.0});
  world.add_bond(0, 1);
  world.add_bond(1, 2);
  BrushAdjacency topology(world);
  BrushPhysicsSettings settings{BrushPhysicsMode::PhysicalAssist, 0.5, 1.0e-6};
  AssistedDab dab;
  dab.drag_delta = {0.0, 2.0, 0.0};
  auto targets = plan_assisted_brush(world, topology, {{0, {}, 0.5}}, dab, settings);
  assert(targets.size() == 1 && near(targets[0].goal.y, 1.0));
  // Assist is blended once in the surface bridge, not in target preparation.
  settings.mode = BrushPhysicsMode::FullPhysics;
  targets = plan_assisted_brush(world, topology, {{0, {}, 0.5}}, dab, settings);
  assert(targets.size() == 1 && near(targets[0].goal.y, 0.5));
  settings.mode = BrushPhysicsMode::Standard;
  assert(plan_assisted_brush(world, topology, {{0, {}, 0.5}}, dab, settings).empty());
  settings.mode = BrushPhysicsMode::PhysicalAssist;
  settings.influence = 0.0;
  assert(plan_assisted_brush(world, topology, {{0, {}, 0.5}}, dab, settings).empty());
  settings.influence = 0.5;

  dab.kind = AssistedBrushKind::Clay;
  dab.normal = {0.0, 0.0, 1.0};
  dab.strength = 0.8;
  targets = plan_assisted_brush(world, topology, {{0, {}, 0.5}}, dab, settings);
  assert(targets.size() == 1 && near(targets[0].goal.z, 0.4));

  dab.kind = AssistedBrushKind::ClayStrips;
  dab.center = {0.0, 0.0, 0.0};
  dab.stroke_direction = {1.0, 0.0, 0.0};
  targets = plan_assisted_brush(world, topology, {{0, {}, 1.0}}, dab, settings);
  assert(targets.size() == 1 && near(targets[0].goal.z, 0.8));
  assert(plan_assisted_brush(world, topology, {{1, {}, 1.0}}, dab, settings).empty());

  dab.kind = AssistedBrushKind::Smooth;
  dab.strength = 1.0;
  targets = plan_assisted_brush(world, topology, {{1, {}, 1.0}}, dab, settings);
  assert(targets.size() == 1 && near(targets[0].goal.y, 0.0));

  bool rejected = false;
  try {
    (void)plan_assisted_brush(world, topology, {{0, {}, 1.0}, {0, {}, 1.0}}, dab, settings);
  } catch (const std::invalid_argument &) { rejected = true; }
  assert(rejected);
  rejected = false;
  try {
    (void)plan_assisted_brush(world, topology,
        {{1, {}, std::numeric_limits<double>::quiet_NaN()}}, dab, settings);
  } catch (const std::invalid_argument &) { rejected = true; }
  assert(rejected);

  world.begin_stroke();
  assert(world.set_brush_targets(targets));
  world.step_local(1.0 / 60.0, 2, 5, 0);
  assert(std::isfinite(world.particles()[1].position.y));
  world.cancel_stroke();
  assert(near(world.particles()[1].position.y, 1.0));
}

static void test_granular()
{
  World world;
  world.add_particle({0.0, 0.0, 0.0}, 1.0, 0.5);
  world.add_particle({0.6, 0.0, 0.0}, 1.0, 0.5);
  GranularContactSettings config;
  auto targets = plan_granular_contacts(world, {1, 0, 1}, config);
  assert(targets.size() == 2);
  assert(targets[0].goal.x < 0.0 && targets[1].goal.x > 0.6);
  assert(near(world.particles()[0].position.x, 0.0));
  world.begin_stroke();
  assert(world.set_brush_targets(targets));
  world.step_local(1.0 / 60.0, 2, 5, 0);
  world.cancel_stroke();
  assert(near(world.particles()[0].position.x, 0.0));

  World wet;
  wet.add_particle({0.0, 0.0, 0.0}, 1.0, 0.5);
  wet.add_particle({1.2, 0.0, 0.0}, 1.0, 0.5);
  config.cohesion = 0.2;
  targets = plan_granular_contacts(wet, {0, 1}, config);
  assert(targets.size() == 2);
  assert(targets[0].goal.x > 0.0 && targets[1].goal.x < 1.2);
  bool rejected = false;
  try { (void)plan_granular_contacts(wet, {0, 3}, config); }
  catch (const std::out_of_range &) { rejected = true; }
  assert(rejected);
}

static void test_multires()
{
  SurfaceAnchor a;
  a.vertices[0] = 0; a.vertices[1] = 1; a.vertices[2] = 2;
  a.weights[0] = 0.2; a.weights[1] = 0.3; a.weights[2] = 0.5;
  SurfaceAnchor b;
  b.vertices[0] = 2; b.vertices[1] = 1; b.vertices[2] = 3;
  b.weights[0] = 0.2; b.weights[1] = 0.3; b.weights[2] = 0.5;
  SurfaceAnchor c = b;
  c.corner_uv[0] = {0.99, 0.11}; // distinct seam data untouched
  MultiresDirtyMap map(4, {a, b, c});
  assert((map.dirty_samples({0}) == std::vector<std::size_t>{0}));
  assert((map.dirty_samples({2, 2}) == std::vector<std::size_t>{0, 1, 2}));
  assert((map.dirty_tiles({3}, 2) == std::vector<std::size_t>{0, 1}));
  assert(map.dirty_samples({}).empty());
  bool rejected = false;
  try { (void)map.dirty_samples({4}); }
  catch (const std::out_of_range &) { rejected = true; }
  assert(rejected);
}

int main()
{
  test_brush_goals();
  test_granular();
  test_multires();
  return 0;
}
