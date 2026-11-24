/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstddef>

namespace sculpt_physics {

/* These enums are planning contracts, not proof a backend is installed.
 * Real Blender adapters must explicitly register runtime capabilities. */
enum class SculptMaterial { Clay, Wax, Mud, Sand, Snow, Stone, Tissue, Cloth };
enum class SolverFamily { XPBDSurface, XPBDVolume, DEMGrains, MPMContinuum };
enum class SolverDevice { CPU, WarpCUDA, VulkanCompute };

struct AvailablePhysics {
  bool cpu_xpbd = true;       // existing CPU World
  bool cpu_volume = false;    // tetra reference exists, but NO integrated volume executor yet
  bool cpu_dem = true;        // simplified granular target planner
  bool mpm_ready = false;     // not integrated: never assume ready
  bool warp_cuda_ready = false; // device/driver/backend initialized
  bool vulkan_ready = false;    // device/driver/backend initialized
  // Device existence does NOT imply support for every physical solver.
  // Set a family flag only after that implementation passes parity tests.
  bool warp_surface_ready = false;
  bool warp_volume_ready = false;
  bool warp_dem_ready = false;
  bool warp_mpm_ready = false;
  bool vulkan_surface_ready = false;
  bool vulkan_volume_ready = false;
  bool vulkan_dem_ready = false;
  bool vulkan_mpm_ready = false;
};

struct SolverRequest {
  SculptMaterial material = SculptMaterial::Clay;
  std::size_t active_proxy_particles = 0;
  std::size_t gpu_break_even_particles = 8192; // uncalibrated; benchmark before enabling
  bool volume_cage_available = false;
  bool prefer_gpu = false;
};

struct SolverPlan {
  SolverFamily family = SolverFamily::XPBDSurface;
  SolverDevice device = SolverDevice::CPU;
  bool implementation_available = false;
  bool approximate_material = false;
};

/* Deterministic, side-effect-free policy helper; no plugin loading or kernel
 * launch. It must never advertise unavailable CUDA/Vulkan/MPM support. */
SolverPlan choose_solver(const SolverRequest &request, const AvailablePhysics &available);

}  // namespace sculpt_physics
