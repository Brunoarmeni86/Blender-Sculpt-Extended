/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "brush_solver_backend.hh"

#include <memory>

namespace sculpt_physics {

std::unique_ptr<BrushSolverBackend> create_warp_cuda_backend();

}  // namespace sculpt_physics
