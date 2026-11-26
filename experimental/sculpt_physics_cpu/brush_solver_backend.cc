/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "brush_solver_backend.hh"
#include "d12_smooth_multiscale.hh"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

#ifdef WITH_SCULPT_WARP_CUDA
#  include "warp_cuda_backend.hh"
#endif

namespace sculpt_physics {
namespace {

class CpuBrushSolverBackend final : public BrushSolverBackend {
 public:
  BackendKind kind() const override
  {
    return BackendKind::Cpu;
  }

  std::size_t add_particle(const Vec3 position, const double inverse_mass) override
  {
    return world_.add_particle(position, inverse_mass);
  }

  void add_bond(const std::size_t a,
                const std::size_t b,
                const double compliance,
                const double yield_strain,
                const double plastic_creep,
                const double fracture_strain) override
  {
    world_.add_bond(a, b, compliance, yield_strain, plastic_creep, fracture_strain);
  }

  void set_velocity_damping(const double damping_per_second) override
  {
    world_.set_velocity_damping(damping_per_second);
  }

  void set_shape_stiffness_override(const double /*stiffness*/) override
  {
    /* The CPU XPBD reference has no vertex-rest Jacobi term. */
  }

  void begin_stroke() override
  {
    world_.begin_stroke();
  }

  bool set_active_region(const std::vector<std::size_t> &particles) override
  {
    return world_.set_active_region(particles);
  }

  bool set_targets(const std::vector<World::BrushTargetInput> &targets) override
  {
    return world_.set_brush_targets(targets);
  }

  void step_local(const double dt,
                  const int substeps,
                  const int iterations,
                  const int bond_hops) override
  {
    world_.step_local(dt, substeps, iterations, bond_hops);
  }

  const std::vector<Particle> &particles() const override
  {
    return world_.particles();
  }

 private:
  World world_;
};

}  // namespace

BackendKind backend_kind_from_name(const std::string_view name)
{
  std::string normalized(name);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return char(std::tolower(c));
  });
  if (normalized.empty() || normalized == "auto") {
    return BackendKind::Auto;
  }
  if (normalized == "cpu") {
    return BackendKind::Cpu;
  }
  if (normalized == "warp" || normalized == "warp-cuda" || normalized == "cuda") {
    return BackendKind::WarpCuda;
  }
  throw std::invalid_argument("unknown sculpt physics backend");
}

std::unique_ptr<BrushSolverBackend> create_brush_solver_backend([[maybe_unused]] const BackendKind requested,
                                                                 BackendKind *selected)
{
#ifdef WITH_SCULPT_WARP_CUDA
  /* Keep Auto on the reference implementation until the native GPU path has
   * passed interactive parity tests.  This also keeps Blender usable on AMD,
   * Intel and NVIDIA systems without the packaged Warp runtime/PTX. */
  if (requested == BackendKind::WarpCuda) {
    try {
      std::unique_ptr<BrushSolverBackend> backend = create_warp_cuda_backend();
      if (selected != nullptr) {
        *selected = BackendKind::WarpCuda;
      }
      return backend;
    }
    catch (const std::exception &) {
      /* Device/module failures retain the CPU reference fallback. */
    }
  }
#endif
  /* Auto and an unavailable explicit GPU request use the reference backend.
   * A backend failure must never disable the brush. */
  if (selected != nullptr) {
    *selected = BackendKind::Cpu;
  }
  return std::make_unique<CpuBrushSolverBackend>();
}

std::shared_ptr<StrokeSolverSession> create_stroke_solver_session(
    const StrokeSolverPurpose purpose,
    const BackendKind requested,
    const std::size_t vertex_count)
{
  auto session = std::make_shared<StrokeSolverSession>();
  session->purpose = purpose;
  session->vertex_count = vertex_count;
  session->solver = create_brush_solver_backend(requested, &session->selected_backend);
  /* Dense sculpt meshes otherwise repeatedly reallocate every particle-side
   * array while preparing the first dab. Triangle topology is bounded near
   * three edges per vertex; unused reserved capacity has no logical size. */
  session->solver->reserve_capacity(vertex_count, vertex_count * 3);
  session->compact_slot.resize(vertex_count);
  session->compact_stamp.assign(vertex_count, 0);
  return session;
}

namespace dedicated_d12::smooth_multiscale {
std::unique_ptr<Backend> create_cuda()
{
#ifdef WITH_SCULPT_WARP_CUDA
  return create_multiscale_cuda_backend();
#else
  return nullptr;
#endif
}
}  // namespace dedicated_d12::smooth_multiscale

}  // namespace sculpt_physics
