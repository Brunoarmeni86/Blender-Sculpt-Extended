/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "solver_policy.hh"

namespace sculpt_physics {

SolverPlan choose_solver(const SolverRequest &r, const AvailablePhysics &a)
{
  SolverPlan plan;
  switch (r.material) {
    case SculptMaterial::Clay:
    case SculptMaterial::Wax:
    case SculptMaterial::Tissue:
      if (r.volume_cage_available && a.cpu_volume) {
        plan.family = SolverFamily::XPBDVolume;
        plan.implementation_available = true;
      }
      else {
        plan.family = SolverFamily::XPBDSurface;
        plan.implementation_available = a.cpu_xpbd;
        plan.approximate_material = true; // no physical volume constraint
      }
      break;
    case SculptMaterial::Mud:
      if (a.mpm_ready && r.volume_cage_available) {
        plan.family = SolverFamily::MPMContinuum;
        plan.implementation_available = true;
      }
      else {
        plan.family = SolverFamily::DEMGrains;
        plan.implementation_available = a.cpu_dem;
        plan.approximate_material = true; // can't model real mud as isolated spheres
      }
      break;
    case SculptMaterial::Sand:
    case SculptMaterial::Snow:
      plan.family = SolverFamily::DEMGrains;
      plan.implementation_available = a.cpu_dem;
      // Simplified DEM contact planner lacks full friction and cohesion history.
      plan.approximate_material = true;
      break;
    case SculptMaterial::Stone:
      plan.family = SolverFamily::XPBDSurface;
      plan.implementation_available = a.cpu_xpbd;
      plan.approximate_material = true; // no crack topology reconstruction
      break;
    case SculptMaterial::Cloth:
      plan.family = SolverFamily::XPBDSurface;
      plan.implementation_available = a.cpu_xpbd;
      plan.approximate_material = true; // bending/self-contact not solved yet
      break;
  }

  // Plan a GPU only when independently verified and profitable under a
  // configured empirical threshold; never assume CUDA or Vulkan from renderer.
  if (plan.implementation_available && r.prefer_gpu &&
      r.gpu_break_even_particles > 0 &&
      r.active_proxy_particles >= r.gpu_break_even_particles) {
    if (a.warp_cuda_ready) plan.device = SolverDevice::WarpCUDA;
    else if (a.vulkan_ready) plan.device = SolverDevice::VulkanCompute;
  }
  return plan;
}

}  // namespace sculpt_physics
