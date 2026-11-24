/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "solver_policy.hh"
#include "volume_constraints.hh"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;

static bool close(double a, double b) { return std::abs(a-b) < 1.0e-8; }

static void test_tetra_volume_reference()
{
  const std::vector<Vec3> rest{
      {0.0,0.0,0.0}, {1.0,0.0,0.0}, {0.0,1.0,0.0}, {0.0,0.0,1.0}};
  assert(close(signed_tetra_volume({rest[0],rest[1],rest[2],rest[3]}), 1.0/6.0));
  auto constraint = make_volume_constraint(rest, {0,1,2,3}, 0.0);
  assert(close(constraint.rest_volume, 1.0/6.0));
  std::vector<TetraVolumeConstraint> tets{constraint};
  auto deformed = rest;
  deformed[3].z = 1.5;
  const double pre = signed_tetra_volume(
      {deformed[0],deformed[1],deformed[2],deformed[3]});
  const auto corrected = solve_tetra_volume_xpbd(
      deformed, {0.0,0.0,0.0,1.0}, tets, 1.0/60.0, 4);
  const double post = signed_tetra_volume(
      {corrected[0],corrected[1],corrected[2],corrected[3]});
  assert(std::abs(post-constraint.rest_volume) < std::abs(pre-constraint.rest_volume));
  assert(close(corrected[3].z, 1.0));
  assert(close(deformed[3].z, 1.5)); // input not mutated
  assert(close(corrected[0].z,0.0) && close(corrected[1].z,0.0) &&
         close(corrected[2].z,0.0));
  assert(std::isfinite(tets[0].lambda));
  const auto previous = tets[0].lambda;
  bool rejected = false;
  try {
    auto invalid = deformed;
    invalid[3].z = std::numeric_limits<double>::quiet_NaN();
    (void)solve_tetra_volume_xpbd(invalid, {0.0,0.0,0.0,1.0},
                                  tets, 1.0/60.0, 2);
  } catch (const std::invalid_argument &) { rejected = true; }
  assert(rejected && close(tets[0].lambda,previous));

  rejected = false;
  try {
    (void)make_volume_constraint(rest,{0,0,2,3},0.0);
  } catch (const std::invalid_argument &) { rejected = true; }
  assert(rejected);
  rejected = false;
  try {
    (void)make_volume_constraint(rest,{0,2,1,3},0.0);
  } catch (const std::invalid_argument &) { rejected = true; }
  assert(rejected);
  // A fully pinned cage is a stable no-op, not a divide by zero.
  tets = {constraint};
  const auto pinned = solve_tetra_volume_xpbd(
      deformed, {0.0,0.0,0.0,0.0}, tets, 1.0/60.0, 2);
  assert(close(pinned[3].z, 1.5));
  // XPBD lambda warm starts are unsafe across unrelated brush time steps.
  // Even with an old nonzero lambda, the rest geometry must not move.
  auto soft = make_volume_constraint(rest,{0,1,2,3},1.0e-5);
  soft.lambda = 1.0;
  std::vector<TetraVolumeConstraint> warm{soft};
  const auto untouched = solve_tetra_volume_xpbd(
      rest, {0.0,0.0,0.0,1.0}, warm, 1.0/60.0, 2);
  assert(close(untouched[3].z,1.0) && close(warm[0].lambda,0.0));
}

static void test_backend_policy()
{
  AvailablePhysics caps;
  SolverRequest clay;
  clay.material = SculptMaterial::Clay;
  auto plan = choose_solver(clay,caps);
  assert(plan.family == SolverFamily::XPBDSurface);
  assert(plan.device == SolverDevice::CPU && plan.approximate_material);
  clay.volume_cage_available = true;
  plan = choose_solver(clay,caps);
  assert(plan.family == SolverFamily::XPBDSurface && plan.approximate_material);
  // A mathematical reference is not an integrated backend. Only an explicit
  // future volume-executor registration can activate this family.
  caps.cpu_volume = true;
  plan = choose_solver(clay,caps);
  assert(plan.family == SolverFamily::XPBDVolume && plan.implementation_available);
  clay.prefer_gpu = true;
  clay.active_proxy_particles = 20000;
  plan = choose_solver(clay,caps);
  assert(plan.device == SolverDevice::CPU); // no GPU implied by user preference
  caps.warp_cuda_ready = true;
  // CUDA device availability alone does not make XPBD volume runnable.
  plan = choose_solver(clay,caps);
  assert(plan.device == SolverDevice::CPU);
  caps.warp_volume_ready = true;
  plan = choose_solver(clay,caps);
  assert(plan.device == SolverDevice::WarpCUDA);
  clay.active_proxy_particles = 100;
  plan = choose_solver(clay,caps);
  assert(plan.device == SolverDevice::CPU);
  caps.warp_cuda_ready = false;
  caps.vulkan_ready = true;
  clay.active_proxy_particles = 20000;
  plan = choose_solver(clay,caps);
  assert(plan.device == SolverDevice::CPU);
  caps.vulkan_volume_ready = true;
  plan = choose_solver(clay,caps);
  assert(plan.device == SolverDevice::VulkanCompute);

  SolverRequest mud;
  mud.material = SculptMaterial::Mud;
  plan = choose_solver(mud,caps);
  assert(plan.family == SolverFamily::DEMGrains && plan.approximate_material);
  caps.mpm_ready = true;
  mud.volume_cage_available = true;
  plan = choose_solver(mud,caps);
  assert(plan.family == SolverFamily::MPMContinuum && !plan.approximate_material);
  SolverRequest sand;
  sand.material = SculptMaterial::Sand;
  plan = choose_solver(sand,caps);
  assert(plan.family == SolverFamily::DEMGrains);
}

int main()
{
  test_tetra_volume_reference();
  test_backend_policy();
  return 0;
}
