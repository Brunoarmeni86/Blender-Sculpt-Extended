/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "brush_solver_backend.hh"
#include "artistic_geometry_v31d1110.hh"
#include "artistic_paired_flux.hh"

#include <memory>

namespace sculpt_physics {

std::unique_ptr<BrushSolverBackend> create_warp_cuda_backend();
std::unique_ptr<artistic_v31d1110::PressureLocalBackend> create_artistic_pressure_cuda();
std::unique_ptr<PairedFluxBackend> create_artistic_flux_cuda();

}  // namespace sculpt_physics
