/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "warp_cuda_backend.hh"

#include "warp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace sculpt_physics {
namespace {

/* Warp modules are process resources, not stroke resources. Loading PTX and
 * creating a CUDA context in every BrushSolverBackend constructor caused the
 * visible pause at the first dab of every Clay/Smooth stroke. Keep them alive
 * for the Blender process; individual stroke buffers remain independently
 * owned and are still released promptly. */
struct SharedWarpRuntime {
  void *context = nullptr;
  void *module = nullptr;
  void *kernel = nullptr;
  void *smooth_module = nullptr;
  void *smooth_kernel = nullptr;
  void *smooth_scatter_kernel = nullptr;
  void *artistic_module = nullptr;
  void *artistic_kernel = nullptr;

  SharedWarpRuntime()
  {
    if (wp_init("1.19.0") != 0) {
      throw std::runtime_error("Warp 1.19 initialization failed");
    }
    if (wp_cuda_device_get_arch(0) < 75) {
      throw std::runtime_error("Warp Sculpt CUDA requires an NVIDIA sm_75 or newer GPU");
    }
    const char *ptx_path = std::getenv("BLENDER_SCULPT_WARP_PTX");
    if (ptx_path == nullptr || ptx_path[0] == '\0') {
      throw std::runtime_error("BLENDER_SCULPT_WARP_PTX is not set");
    }
    context = wp_cuda_context_create(0);
    if (context == nullptr) {
      throw std::runtime_error("Warp could not create CUDA context 0");
    }
    module = wp_cuda_load_module(context, ptx_path);
    if (module == nullptr) {
      throw std::runtime_error("Warp could not load the Sculpt Jacobi PTX module");
    }
    kernel = wp_cuda_get_kernel(
        context, module, "jacobi_shape_step_2135c77f_cuda_kernel_forward");
    if (kernel == nullptr) {
      throw std::runtime_error("Warp Sculpt Jacobi kernel is missing from PTX module");
    }
    const char *artistic_path=std::getenv("BLENDER_SCULPT_D11_WARP_PTX");
    const char *artistic_symbol=std::getenv("BLENDER_SCULPT_D11_WARP_KERNEL");
    if(artistic_path && artistic_path[0] && artistic_symbol && artistic_symbol[0]){
      artistic_module=wp_cuda_load_module(context,artistic_path);
      if(artistic_module)
        artistic_kernel=wp_cuda_get_kernel(context,artistic_module,artistic_symbol);
    }
    const char *smooth_ptx_path = std::getenv("BLENDER_SCULPT_WARP_SMOOTH_PTX");
    if (smooth_ptx_path != nullptr && smooth_ptx_path[0] != '\0') {
      smooth_module = wp_cuda_load_module(context, smooth_ptx_path);
      if (smooth_module != nullptr) {
        smooth_kernel = wp_cuda_get_kernel(
            context, smooth_module, "sculpt_smooth_laplacian_b88c9770_cuda_kernel_forward");
        smooth_scatter_kernel = wp_cuda_get_kernel(
            context, smooth_module, "sculpt_smooth_scatter_4021f8a1_cuda_kernel_forward");
      }
    }
  }
};

static SharedWarpRuntime &shared_warp_runtime()
{
  static SharedWarpRuntime runtime;
  return runtime;
}

class WarpCudaBrushSolverBackend final : public BrushSolverBackend {
 public:
  WarpCudaBrushSolverBackend()
  {
    SharedWarpRuntime &runtime = shared_warp_runtime();
    context_ = runtime.context;
    module_ = runtime.module;
    kernel_ = runtime.kernel;
    smooth_module_ = runtime.smooth_module;
    smooth_kernel_ = runtime.smooth_kernel;
    smooth_scatter_kernel_ = runtime.smooth_scatter_kernel;
  }

  ~WarpCudaBrushSolverBackend() override
  {
    release_buffers();
  }

  BackendKind kind() const override
  {
    return BackendKind::WarpCuda;
  }

  void reserve_capacity(const std::size_t particles, const std::size_t bonds) override
  {
    particles_.reserve(particles);
    rest_.reserve(particles);
    inverse_mass_.reserve(particles);
    yield_strain_.reserve(particles);
    plastic_creep_.reserve(particles);
    rest_scale_sum_.reserve(particles);
    rest_scale_count_.reserve(particles);
    bonds_.reserve(bonds);
  }

  bool synchronize_positions(const std::vector<Vec3> &positions) override
  {
    if (!uploaded_ || positions.size() != particles_.size()) {
      return false;
    }
    for (std::size_t i = 0; i < positions.size(); i++) {
      particles_[i].position = positions[i];
      const Float3 p = {
          float(positions[i].x), float(positions[i].y), float(positions[i].z)};
      host_positions_[i] = p;
      rest_[i] = p;
    }
    return copy_h2d(d_position_a_, host_positions_) && copy_h2d(d_position_b_, host_positions_) &&
           copy_h2d(d_rest_, rest_);
  }

  bool synchronize_positions_compact(const std::vector<std::size_t> &indices,
                                     const std::vector<Vec3> &positions,
                                     const bool update_rest) override
  {
    if (!uploaded_ || indices.empty() || indices.size() != positions.size() ||
        smooth_scatter_kernel_ == nullptr)
    {
      return false;
    }
    smooth_active_.resize(indices.size());
    smooth_output_.resize(positions.size());
    for (std::size_t i = 0; i < indices.size(); i++) {
      if (indices[i] >= particles_.size()) {
        return false;
      }
      smooth_active_[i] = int(indices[i]);
      smooth_output_[i] = {
          float(positions[i].x), float(positions[i].y), float(positions[i].z)};
      particles_[indices[i]].position = positions[i];
      rest_[indices[i]] = smooth_output_[i];
    }
    if (!copy_h2d(d_active_indices_, smooth_active_) ||
        !copy_h2d(d_smooth_output_, smooth_output_))
    {
      return false;
    }
    wp::array_t<int> compact_indices(static_cast<int *>(d_active_indices_), int(indices.size()));
    wp::array_t<wp::vec3> compact_positions(static_cast<wp::vec3 *>(d_smooth_output_),
                                             int(indices.size()));
    wp::launch_bounds_t<1> bounds;
    bounds.shape[0] = uint32_t(indices.size());
    bounds.size = indices.size();
    bounds.coord_mult = 1;
    /* The physical finish reads position A directly. Its session is rebuilt
     * before a later XPBD material operation, so updating rest here would be
     * a redundant GPU launch on every dab. Clay Strips explicitly keeps rest
     * synchronized because its constraint solve consumes it. */
    const std::array<void *, 2> destinations = {d_position_a_, d_rest_};
    const int destination_count = update_rest ? 2 : 1;
    for (int destination_i = 0; destination_i < destination_count; destination_i++) {
      void *destination = destinations[destination_i];
      wp::array_t<wp::vec3> output(static_cast<wp::vec3 *>(destination), int(particles_.size()));
      void *args[] = {&bounds, &compact_indices, &compact_positions, &output};
      if (wp_cuda_launch_kernel(context_,
                                smooth_scatter_kernel_,
                                indices.size(),
                                0,
                                256,
                                1,
                                1,
                                0,
                                args,
                                nullptr,
                                nullptr) != 0)
      {
        return false;
      }
    }
    return true;
  }

  std::size_t add_particle(const Vec3 position, const double inverse_mass) override
  {
    if (uploaded_) {
      throw std::logic_error("cannot add Warp particles after begin_stroke");
    }
    Particle particle;
    particle.position = position;
    particle.inverse_mass = inverse_mass;
    particles_.push_back(particle);
    rest_.push_back({float(position.x), float(position.y), float(position.z)});
    inverse_mass_.push_back(float(inverse_mass));
    yield_strain_.push_back(std::numeric_limits<float>::infinity());
    plastic_creep_.push_back(0.0f);
    rest_scale_sum_.push_back(0.0f);
    rest_scale_count_.push_back(0);
    return particles_.size() - 1;
  }

  void add_bond(const std::size_t a,
                const std::size_t b,
                const double compliance,
                const double yield_strain,
                const double plastic_creep,
                const double /*fracture_strain*/) override
  {
    if (uploaded_ || a >= particles_.size() || b >= particles_.size() || a == b) {
      throw std::invalid_argument("invalid Warp bond");
    }
    bonds_.push_back({int(a), int(b)});
    compliance_sum_ += std::max(0.0, compliance);
    const Vec3 delta = particles_[b].position - particles_[a].position;
    const float length = float(std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
    if (length > 1.0e-12f && std::isfinite(length)) {
      edge_length_sum_ += length;
      edge_length_count_++;
    }
    for (const std::size_t vertex : {a, b}) {
      if (std::isfinite(yield_strain)) {
        yield_strain_[vertex] = std::min(yield_strain_[vertex], float(std::max(0.0, yield_strain)));
      }
      plastic_creep_[vertex] = std::max(plastic_creep_[vertex], float(std::max(0.0, plastic_creep)));
      rest_scale_sum_[vertex] += length;
      rest_scale_count_[vertex]++;
    }
  }

  void set_velocity_damping(const double damping_per_second) override
  {
    damping_ = std::max(0.0, damping_per_second);
  }

  void set_shape_stiffness_override(const double stiffness) override
  {
    shape_stiffness_override_ = std::clamp(stiffness, -1.0, 1.0);
  }

  void begin_stroke() override
  {
    build_adjacency();
    allocate_and_upload_static_data();
    uploaded_ = true;
  }

  bool smooth_compact(const std::vector<std::size_t> &active,
                      const std::vector<float> &weights,
                      const std::vector<Vec3> &normals,
                      const SmoothSolverMode mode,
                      const double spatial_scale) override
  {
    if (!uploaded_ || smooth_kernel_ == nullptr || smooth_scatter_kernel_ == nullptr ||
        active.empty() || active.size() != weights.size() || active.size() != normals.size() ||
        active.size() > particles_.size())
    {
      return false;
    }
    smooth_active_.resize(active.size());
    smooth_weights_ = weights;
    smooth_normals_.resize(normals.size());
    for (std::size_t i = 0; i < active.size(); i++) {
      if (active[i] >= particles_.size()) {
        return false;
      }
      smooth_active_[i] = int(active[i]);
      smooth_normals_[i] = {
          float(normals[i].x), float(normals[i].y), float(normals[i].z)};
    }
    if (!copy_h2d(d_active_indices_, smooth_active_) ||
        !copy_h2d(d_smooth_weights_, smooth_weights_) ||
        !copy_h2d(d_smooth_normals_, smooth_normals_))
    {
      return false;
    }

    wp::array_t<wp::vec3> positions(static_cast<wp::vec3 *>(d_position_a_), int(particles_.size()));
    wp::array_t<int> offsets(static_cast<int *>(d_offsets_), int(offsets_.size()));
    wp::array_t<int> neighbors(static_cast<int *>(d_neighbors_), int(neighbors_.size()));
    wp::array_t<int> indices(static_cast<int *>(d_active_indices_), int(active.size()));
    wp::array_t<float> factors(static_cast<float *>(d_smooth_weights_), int(active.size()));
    wp::array_t<wp::vec3> active_normals(static_cast<wp::vec3 *>(d_smooth_normals_),
                                         int(active.size()));
    wp::array_t<wp::vec3> compact(static_cast<wp::vec3 *>(d_smooth_output_), int(active.size()));
    wp::launch_bounds_t<1> bounds;
    bounds.shape[0] = uint32_t(active.size());
    bounds.size = active.size();
    bounds.coord_mult = 1;
    int kernel_mode = int(mode);
    float reference_edge = edge_length_count_ > 0 ?
                               float(edge_length_sum_ / double(edge_length_count_)) :
                               1.0f;
    const double edge_ratio = std::max(1.0, spatial_scale / double(reference_edge));
    /* Dense Strong uses a two-ring kernel, so each launch covers roughly four
     * times the diffusion area of the one-ring modes. Bound the launch count
     * while retaining scale equivalence across common sculpt densities. */
    const int requested_density_passes = std::clamp(
        int(std::ceil(edge_ratio * edge_ratio * 0.004)), 3, 12);
    /* Bound latency by the actual edited region, not total mesh size. Large
     * brushes receive fewer interactive passes; continuous dabs accumulate
     * the same response without producing a long single-frame stall. */
    const int interactive_pass_cap = active.size() > 500000 ? 4 :
                                     active.size() > 200000 ? 6 :
                                     active.size() > 75000  ? 8 :
                                                               12;
    const int density_passes = std::min(requested_density_passes, interactive_pass_cap);
    const int passes = mode == SmoothSolverMode::Taubin ? 2 :
                       mode == SmoothSolverMode::DenseStrong ? density_passes :
                                                               1;
    for (int pass = 0; pass < passes; pass++) {
      /* A net-positive Taubin pair preserves volume without cancelling almost
       * the whole continuous stroke. Dense Strong performs three bounded
       * diffusion passes, approximating the world-space response of a mesh
       * with roughly one third as many surface samples. */
      float pass_scale = mode == SmoothSolverMode::Taubin ? (pass == 0 ? 0.62f : -0.34f) : 1.0f;
      void *smooth_args[] = {&bounds,
                             &positions,
                             &offsets,
                             &neighbors,
                             &indices,
                             &factors,
                             &active_normals,
                             &kernel_mode,
                             &pass_scale,
                             &reference_edge,
                             &compact};
      if (wp_cuda_launch_kernel(context_, smooth_kernel_, active.size(), 0, 256, 1, 1, 0,
                                smooth_args, nullptr, nullptr) != 0)
      {
        return false;
      }
      void *scatter_args[] = {&bounds, &indices, &compact, &positions};
      if (wp_cuda_launch_kernel(context_, smooth_scatter_kernel_, active.size(), 0, 256, 1, 1, 0,
                                scatter_args, nullptr, nullptr) != 0)
      {
        return false;
      }
    }
    smooth_output_.resize(active.size());
    if (!wp_cuda_context_synchronize(context_) ||
        !wp_memcpy_d2h(context_,
                       smooth_output_.data(),
                       d_smooth_output_,
                       sizeof(Float3) * smooth_output_.size()))
    {
      return false;
    }
    for (std::size_t i = 0; i < active.size(); i++) {
      const Float3 &p = smooth_output_[i];
      particles_[active[i]].position = {p.x, p.y, p.z};
    }
    return true;
  }

  bool set_active_region(const std::vector<std::size_t> &particles) override
  {
    if (!uploaded_) {
      throw std::logic_error("Warp active region requires begin_stroke");
    }
    active_indices_.clear();
    active_indices_.reserve(particles.size());
    active_inverse_mass_.assign(particles_.size(), 0.0f);
    for (const std::size_t i : particles) {
      if (i >= particles_.size()) {
        return false;
      }
      active_indices_.push_back(int(i));
      active_inverse_mass_[i] = inverse_mass_[i];
    }
    std::sort(active_indices_.begin(), active_indices_.end());
    active_indices_.erase(std::unique(active_indices_.begin(), active_indices_.end()),
                          active_indices_.end());
    return true;
  }

  bool set_targets(const std::vector<World::BrushTargetInput> &targets) override
  {
    if (!uploaded_) {
      throw std::logic_error("Warp targets require begin_stroke");
    }
    targets_ = rest_;
    target_weights_.assign(particles_.size(), 0.0f);
    if (active_indices_.empty()) {
      active_inverse_mass_.assign(particles_.size(), 0.0f);
      active_indices_.reserve(targets.size());
    }
    for (const World::BrushTargetInput &target : targets) {
      if (target.particle_index >= particles_.size()) {
        return false;
      }
      const std::size_t i = target.particle_index;
      targets_[i] = {float(target.goal.x), float(target.goal.y), float(target.goal.z)};
      const double compliance = std::max(0.0, target.compliance);
      /* Zero is required by GPU Smooth for the edge of the brush falloff.
       * Grab/Clay already omit zero-influence targets, so widening this range
       * does not change their normal target weights. */
      target_weights_[i] = float(std::clamp(1.0 / (1.0 + compliance * 250000.0), 0.0, 1.0));
      if (active_inverse_mass_[i] == 0.0f && inverse_mass_[i] > 0.0f) {
        active_inverse_mass_[i] = inverse_mass_[i];
        active_indices_.push_back(int(i));
      }
    }
    std::sort(active_indices_.begin(), active_indices_.end());
    active_indices_.erase(std::unique(active_indices_.begin(), active_indices_.end()),
                          active_indices_.end());
    return copy_h2d(d_targets_, targets_) && copy_h2d(d_target_weights_, target_weights_) &&
           copy_h2d(d_active_inverse_mass_, active_inverse_mass_) &&
           copy_h2d(d_active_indices_, active_indices_);
  }

  void step_local(const double dt,
                  const int /*substeps*/,
                  const int iterations,
                  const int /*bond_hops*/) override
  {
    using Clock = std::chrono::steady_clock;
    const bool profile = std::getenv("BLENDER_SCULPT_XPBD_GPU_PROFILE") != nullptr;
    const auto started = Clock::now();
    if (active_indices_.empty()) {
      return;
    }
    /* The caller owns the quality budget. The earlier hard minimum of twelve
     * launches made a one-pass Smooth dab twelve times more expensive and
     * prevented Clay's density-adaptive budget from working on CUDA. */
    const int solve_iterations = std::max(1, iterations);
    const double mean_compliance = bonds_.empty() ? 0.0 : compliance_sum_ / bonds_.size();
    const float stiffness = float(std::clamp(0.92 / (1.0 + mean_compliance * 20000.0), 0.12, 0.92));
    const float damping_relaxation = float(std::clamp(std::exp(-damping_ * std::max(dt, 0.0)), 0.15, 1.0));
    for (int iteration = 0; iteration < solve_iterations; iteration++) {
      wp::array_t<wp::vec3> positions(static_cast<wp::vec3 *>(d_position_a_), int(particles_.size()));
      wp::array_t<wp::vec3> rest(static_cast<wp::vec3 *>(d_rest_), int(particles_.size()));
      wp::array_t<float> mass(static_cast<float *>(d_active_inverse_mass_), int(particles_.size()));
      wp::array_t<int> offsets(static_cast<int *>(d_offsets_), int(offsets_.size()));
      wp::array_t<int> neighbors(static_cast<int *>(d_neighbors_), int(neighbors_.size()));
      wp::array_t<wp::vec3> targets(static_cast<wp::vec3 *>(d_targets_), int(particles_.size()));
      wp::array_t<float> weights(static_cast<float *>(d_target_weights_), int(particles_.size()));
      wp::array_t<int> active(static_cast<int *>(d_active_indices_), int(active_indices_.size()));
      wp::array_t<wp::vec3> output(static_cast<wp::vec3 *>(d_position_b_), int(particles_.size()));
      float material_stiffness = shape_stiffness_override_ >= 0.0 ?
                                     float(shape_stiffness_override_) :
                                     stiffness * damping_relaxation;
      wp::launch_bounds_t<1> bounds;
      bounds.shape[0] = uint32_t(particles_.size());
      bounds.size = particles_.size();
      bounds.coord_mult = 1;
      void *args[] = {&bounds,
                      &positions,
                      &rest,
                      &mass,
                      &offsets,
                      &neighbors,
                      &targets,
                      &weights,
                      &active,
                      &material_stiffness,
                      &output};
      if (wp_cuda_launch_kernel(context_,
                                kernel_,
                                active_indices_.size(),
                                0,
                                256,
                                1,
                                1,
                                0,
                                args,
                                nullptr,
                                nullptr) != 0)
      {
        throw std::runtime_error("Warp Sculpt Jacobi kernel launch failed");
      }
      std::swap(d_position_a_, d_position_b_);
    }
    const auto launched = Clock::now();
    if (!wp_cuda_context_synchronize(context_) ||
        !wp_memcpy_d2h(context_, host_positions_.data(), d_position_a_, bytes(rest_)))
    {
      throw std::runtime_error("Warp Sculpt result download failed");
    }
    for (const int i : active_indices_) {
      if (i >= 0 && std::size_t(i) < particles_.size()) {
        particles_[i].position = {
            host_positions_[i].x, host_positions_[i].y, host_positions_[i].z};
      }
    }
    /* The CPU reference stores plasticity per distance bond. The current Warp
     * Jacobi solver represents shape through vertex rest positions, so retain
     * the same yield/creep semantics by advancing the persistent local rest
     * shape after the solve. This is intentionally conservative and only runs
     * for material vertices whose bonds requested plasticity. The updated rest
     * state remains resident for subsequent dabs in the same stroke. */
    bool plastic_rest_changed = false;
    const float plastic_dt = float(std::max(0.0, dt));
    for (const int vertex : active_indices_) {
      if (vertex < 0 || std::size_t(vertex) >= rest_.size() ||
          plastic_creep_[vertex] <= 0.0f || !std::isfinite(yield_strain_[vertex]))
      {
        continue;
      }
      const Float3 &position = host_positions_[vertex];
      Float3 &rest = rest_[vertex];
      const float dx = position.x - rest.x;
      const float dy = position.y - rest.y;
      const float dz = position.z - rest.z;
      const float displacement = std::sqrt(dx * dx + dy * dy + dz * dz);
      const float scale = rest_scale_count_[vertex] > 0 ?
                              rest_scale_sum_[vertex] / float(rest_scale_count_[vertex]) :
                              0.0f;
      if (scale <= 1.0e-12f || displacement / scale <= yield_strain_[vertex]) {
        continue;
      }
      const float rate = 1.0f - std::exp(-plastic_creep_[vertex] * plastic_dt);
      rest.x += dx * rate;
      rest.y += dy * rate;
      rest.z += dz * rate;
      plastic_rest_changed = true;
    }
    if (plastic_rest_changed && !copy_h2d(d_rest_, rest_)) {
      throw std::runtime_error("Warp Sculpt plastic rest upload failed");
    }
    if (profile) {
      const auto finished = Clock::now();
      std::fprintf(stderr,
                   "[XPBD GPU] particles=%zu iterations=%d launch_ms=%.3f sync_download_ms=%.3f stiffness=%.3f damping=%.3f\n",
                   active_indices_.size(), solve_iterations,
                   std::chrono::duration<double, std::milli>(launched - started).count(),
                   std::chrono::duration<double, std::milli>(finished - launched).count(),
                   stiffness, damping_);
    }
  }

  const std::vector<Particle> &particles() const override
  {
    return particles_;
  }

 private:
  struct Float3 {
    float x, y, z;
  };

  template<typename T> static std::size_t bytes(const std::vector<T> &values)
  {
    return sizeof(T) * values.size();
  }

  template<typename T> bool copy_h2d(void *destination, std::vector<T> &source)
  {
    return wp_memcpy_h2d(context_, destination, source.data(), bytes(source));
  }

  void build_adjacency()
  {
    /* Two linear passes avoid one heap allocation per vertex. On multi-million
     * vertex meshes this is a substantial part of the first-dab pause. */
    offsets_.assign(particles_.size() + 1, 0);
    for (const auto &bond : bonds_) {
      offsets_[std::size_t(bond.first) + 1]++;
      offsets_[std::size_t(bond.second) + 1]++;
    }
    for (std::size_t i = 1; i < offsets_.size(); i++) {
      offsets_[i] += offsets_[i - 1];
    }
    neighbors_.assign(std::size_t(offsets_.back()), -1);
    std::vector<int> cursor(offsets_.begin(), offsets_.end() - 1);
    for (const auto &bond : bonds_) {
      neighbors_[cursor[std::size_t(bond.first)]++] = bond.second;
      neighbors_[cursor[std::size_t(bond.second)]++] = bond.first;
    }
  }

  void allocate_and_upload_static_data()
  {
    release_buffers();
    host_positions_ = rest_;
    targets_ = rest_;
    target_weights_.assign(particles_.size(), 0.0f);
    active_inverse_mass_.assign(particles_.size(), 0.0f);
    d_position_a_ = wp_alloc_device(context_, bytes(rest_));
    d_position_b_ = wp_alloc_device(context_, bytes(rest_));
    d_rest_ = wp_alloc_device(context_, bytes(rest_));
    d_active_inverse_mass_ = wp_alloc_device(context_, bytes(inverse_mass_));
    d_offsets_ = wp_alloc_device(context_, bytes(offsets_));
    d_neighbors_ = wp_alloc_device(context_, bytes(neighbors_));
    d_targets_ = wp_alloc_device(context_, bytes(targets_));
    d_target_weights_ = wp_alloc_device(context_, bytes(target_weights_));
    d_active_indices_ = wp_alloc_device(context_, sizeof(int) * particles_.size());
    d_smooth_weights_ = wp_alloc_device(context_, sizeof(float) * particles_.size());
    d_smooth_output_ = wp_alloc_device(context_, sizeof(Float3) * particles_.size());
    d_smooth_normals_ = wp_alloc_device(context_, sizeof(Float3) * particles_.size());
    if (!d_position_a_ || !d_position_b_ || !d_rest_ || !d_active_inverse_mass_ || !d_offsets_ ||
        !d_neighbors_ || !d_targets_ || !d_target_weights_ || !d_active_indices_ ||
        !d_smooth_weights_ || !d_smooth_output_ || !d_smooth_normals_ ||
        !copy_h2d(d_position_a_, rest_) ||
        !copy_h2d(d_position_b_, rest_) || !copy_h2d(d_rest_, rest_) ||
        !copy_h2d(d_offsets_, offsets_) || !copy_h2d(d_neighbors_, neighbors_))
    {
      throw std::runtime_error("Warp Sculpt GPU allocation/upload failed");
    }
  }

  void release_buffers()
  {
    for (void **pointer : {&d_position_a_,
                           &d_position_b_,
                           &d_rest_,
                           &d_active_inverse_mass_,
                           &d_offsets_,
                           &d_neighbors_,
                           &d_targets_,
                           &d_target_weights_,
                           &d_active_indices_,
                           &d_smooth_weights_,
                           &d_smooth_output_,
                           &d_smooth_normals_})
    {
      if (*pointer != nullptr && context_ != nullptr) {
        wp_free_device(context_, *pointer);
        *pointer = nullptr;
      }
    }
  }

  void *context_ = nullptr;
  void *module_ = nullptr;
  void *kernel_ = nullptr;
  void *smooth_module_ = nullptr;
  void *smooth_kernel_ = nullptr;
  void *smooth_scatter_kernel_ = nullptr;
  bool uploaded_ = false;
  double damping_ = 0.0;
  double shape_stiffness_override_ = -1.0;
  double compliance_sum_ = 0.0;
  double edge_length_sum_ = 0.0;
  std::size_t edge_length_count_ = 0;
  std::vector<Particle> particles_;
  std::vector<Float3> rest_;
  std::vector<Float3> host_positions_;
  std::vector<Float3> targets_;
  std::vector<float> inverse_mass_;
  std::vector<float> yield_strain_;
  std::vector<float> plastic_creep_;
  std::vector<float> rest_scale_sum_;
  std::vector<int> rest_scale_count_;
  std::vector<float> active_inverse_mass_;
  std::vector<float> target_weights_;
  std::vector<int> active_indices_;
  std::vector<int> smooth_active_;
  std::vector<float> smooth_weights_;
  std::vector<Float3> smooth_output_;
  std::vector<Float3> smooth_normals_;
  std::vector<std::pair<int, int>> bonds_;
  std::vector<int> offsets_;
  std::vector<int> neighbors_;
  void *d_position_a_ = nullptr;
  void *d_position_b_ = nullptr;
  void *d_rest_ = nullptr;
  void *d_active_inverse_mass_ = nullptr;
  void *d_offsets_ = nullptr;
  void *d_neighbors_ = nullptr;
  void *d_targets_ = nullptr;
  void *d_target_weights_ = nullptr;
  void *d_active_indices_ = nullptr;
  void *d_smooth_weights_ = nullptr;
  void *d_smooth_output_ = nullptr;
  void *d_smooth_normals_ = nullptr;
};

// Independent pressure proposals. Reuse device buffers throughout one stroke;
// only the bounded local patch is uploaded and downloaded. No editor writes.
class ArtisticPressureCuda final : public artistic_v31d1110::PressureLocalBackend {
  using Float3=std::array<float,3>;
  static_assert(sizeof(Float3)==sizeof(wp::vec3));
  void *context_=nullptr,*kernel_=nullptr;
  std::array<void*,6> device_{};
  std::size_t capacity_=0;
  bool failed_=false;
  std::vector<Float3> p_,n_,motion_;
  std::vector<float> weight_,compaction_,drive_;
  void release(){
    for(auto& d:device_)if(d){wp_free_device(context_,d);d=nullptr;}
    capacity_=0;
  }
  bool reserve(std::size_t count){
    if(capacity_>=count)return true;
    release();
    const auto capacity=std::min<std::size_t>(8192,std::max<std::size_t>(256,count*2));
    const std::array<std::size_t,6> stride{sizeof(Float3),sizeof(Float3),sizeof(float),
                                          sizeof(float),sizeof(Float3),sizeof(float)};
    for(std::size_t i=0;i<device_.size();++i){
      device_[i]=wp_alloc_device(context_,stride[i]*capacity);
      if(!device_[i]){release();return false;}
    }
    capacity_=capacity;
    return true;
  }
  template<class T> bool upload(std::size_t i,std::vector<T>& v){
    return wp_memcpy_h2d(context_,device_[i],v.data(),sizeof(T)*v.size());
  }
 public:
  ArtisticPressureCuda(){
    auto& runtime=shared_warp_runtime();
    context_=runtime.context;kernel_=runtime.artistic_kernel;
    if(!kernel_)throw std::runtime_error("D11 artistic Warp module unavailable");
  }
  ~ArtisticPressureCuda() override {release();}
  bool evaluate_local(const artistic_v31d1110::h::Patch& patch,
      artistic_v31d1110::Action action,artistic_v31d1110::Material material,
      Vec3 center,Vec3 axis,double radius,double dose,double pressure,double plane_height,
      const std::vector<double>& compaction,std::vector<Vec3>& motion,
      std::vector<double>& drive) override {
    namespace a=artistic_v31d1110;
    const auto count=patch.p.size();
    if(failed_||count==0||count>8192||compaction.size()!=count||
       patch.normal.size()!=count||patch.weight.size()!=count)return false;
    // Refuse small patches: dispatch + transfer costs dominate.
    if(count<512)return false;
    if(!reserve(count)){failed_=true;return false;}
    p_.resize(count);n_.resize(count);weight_.resize(count);compaction_.resize(count);
    motion_.resize(count);drive_.resize(count);
    for(std::size_t i=0;i<count;++i){
      const auto q=patch.p[i]-center,v=patch.normal[i];
      p_[i]={float(q.x),float(q.y),float(q.z)};
      n_[i]={float(v.x),float(v.y),float(v.z)};
      weight_[i]=float(patch.weight[i]);compaction_[i]=float(compaction[i]);
      for(float x:{p_[i][0],p_[i][1],p_[i][2],n_[i][0],n_[i][1],n_[i][2],
                    weight_[i],compaction_[i]})if(!std::isfinite(x))return false;
    }
    if(!upload(0,p_)||!upload(1,n_)||!upload(2,weight_)||!upload(3,compaction_)){
      failed_=true;return false;
    }
    const int size=int(count);
    wp::array_t<wp::vec3> p(static_cast<wp::vec3*>(device_[0]),size);
    wp::array_t<wp::vec3> n(static_cast<wp::vec3*>(device_[1]),size);
    wp::array_t<float> w(static_cast<float*>(device_[2]),size);
    wp::array_t<float> c(static_cast<float*>(device_[3]),size);
    wp::array_t<wp::vec3> out(static_cast<wp::vec3*>(device_[4]),size);
    wp::array_t<float> d(static_cast<float*>(device_[5]),size);
    wp::launch_bounds_t<1> bounds;
    bounds.shape[0]=uint32_t(count);bounds.size=count;bounds.coord_mult=1;
    auto profile=a::profile(material);
    int op=int(action);
    Float3 axis_f{float(axis.x),float(axis.y),float(axis.z)};
    float r=float(radius),e=float(dose),q=float(pressure),plane=float(plane_height);
    float yield_value=float(profile.yield),flow=float(profile.flow);
    float compression=float(profile.compression),recovery=float(profile.recovery);
    float hardening=float(profile.hardening);
    if(!std::isfinite(r)||r<=0||!std::isfinite(e)||!std::isfinite(q)||!std::isfinite(plane))
      return false;
    void *args[]={&bounds,&p,&n,&w,&c,&op,&axis_f,&r,&e,&q,&plane,
                  &yield_value,&flow,&compression,&recovery,&hardening,&out,&d};
    if(wp_cuda_launch_kernel(context_,kernel_,count,0,256,1,1,0,args,nullptr,nullptr)!=0 ||
       !wp_cuda_context_synchronize(context_) ||
       !wp_memcpy_d2h(context_,motion_.data(),device_[4],sizeof(Float3)*count) ||
       !wp_memcpy_d2h(context_,drive_.data(),device_[5],sizeof(float)*count)){
      failed_=true;return false;
    }
    motion.resize(count);drive.resize(count);
    for(std::size_t i=0;i<count;++i){
      motion[i]={motion_[i][0],motion_[i][1],motion_[i][2]};
      drive[i]=drive_[i];
    }
    return true;
  }
};

}  // namespace

std::unique_ptr<artistic_v31d1110::PressureLocalBackend> create_artistic_pressure_cuda()
{
  try{return std::make_unique<ArtisticPressureCuda>();}
  catch(const std::exception&){return nullptr;}
}

std::unique_ptr<BrushSolverBackend> create_warp_cuda_backend()
{
  return std::make_unique<WarpCudaBrushSolverBackend>();
}

}  // namespace sculpt_physics
