/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "brush_solver_backend.hh"

#include <cassert>
#include <cmath>
#include <stdexcept>

using namespace sculpt_physics;

int main()
{
  assert(backend_kind_from_name("AUTO") == BackendKind::Auto);
  assert(backend_kind_from_name("cpu") == BackendKind::Cpu);
  assert(backend_kind_from_name("warp-cuda") == BackendKind::WarpCuda);
  bool rejected = false;
  try {
    (void)backend_kind_from_name("not-a-backend");
  }
  catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);

  BackendKind selected = BackendKind::Auto;
  std::unique_ptr<BrushSolverBackend> backend = create_brush_solver_backend(
      BackendKind::WarpCuda, &selected);
  assert(backend);
  assert(selected == BackendKind::Cpu);
  assert(backend->kind() == BackendKind::Cpu);

  backend->add_particle({0.0, 0.0, 0.0}, 1.0);
  backend->add_particle({1.0, 0.0, 0.0}, 1.0);
  backend->add_bond(0, 1, 1.0e-5);
  backend->begin_stroke();
  assert(backend->set_targets({{0, {0.25, 0.0, 0.0}, 1.0e-6}}));
  backend->step_local(1.0 / 60.0, 2, 5, 0);
  assert(backend->particles().size() == 2);
  assert(std::isfinite(backend->particles()[0].position.x));
  assert(backend->particles()[0].position.x > 0.0);
  return 0;
}
