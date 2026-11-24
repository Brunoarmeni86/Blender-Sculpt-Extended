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
    bool warp_family = false;
    bool vulkan_family = false;
    switch (plan.family) {
      case SolverFamily::XPBDSurface:
        warp_family = a.warp_surface_ready;
        vulkan_family = a.vulkan_surface_ready;
        break;
      case SolverFamily::XPBDVolume:
        warp_family = a.warp_volume_ready;
        vulkan_family = a.vulkan_volume_ready;
        break;
      case SolverFamily::DEMGrains:
        warp_family = a.warp_dem_ready;
        vulkan_family = a.vulkan_dem_ready;
        break;
      case SolverFamily::MPMContinuum:
        warp_family = a.warp_mpm_ready;
        vulkan_family = a.vulkan_mpm_ready;
        break;
    }
    if (a.warp_cuda_ready && warp_family) plan.device = SolverDevice::WarpCUDA;
    else if (a.vulkan_ready && vulkan_family) plan.device = SolverDevice::VulkanCompute;
  }
  return plan;
}

}  // namespace sculpt_physics
