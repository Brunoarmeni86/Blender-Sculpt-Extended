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
  bool cpu_volume = true;     // experimental tetra constraint reference
  bool cpu_dem = true;        // simplified granular target planner
  bool mpm_ready = false;     // not integrated: never assume ready
  bool warp_cuda_ready = false; // only true after device+kernel parity tests
  bool vulkan_ready = false;  // not integrated
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
