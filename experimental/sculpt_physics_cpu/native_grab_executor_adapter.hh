/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_executor.hh"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace sculpt_physics {

/* Blender has already evaluated exact pressure, mask, face-set masking,
 * texture/falloff, silhouette and other native factors for these vertices.
 * Do not multiply in Brush strength or Assist influence a second time. */
struct NativeGrabSample {
  std::size_t global_index = 0;
  double factor = 0.0; // final Blender influence; must lie in [0,1]
};

struct NativeGrabDab {
  Vec3 delta{}; // incremental Grab displacement in mesh/object space
  double compliance = 1.0e-6;
  double dt = 1.0 / 60.0;
  int substeps = 2;
  int iterations = 5;
  std::uint64_t topology_generation = 0;
};

/* RAII staging only. No Blender editor writes occur here.
 * Editor writes are owned by the caller and MUST happen atomically under
 * the caller's native Sculpt Undo transaction before finish_editor_success.
 * On exceptions before that point, the isolated XPBD state is discarded. */
class PreparedNativeGrabDab {
 public:
  PreparedNativeGrabDab() = default;
  PreparedNativeGrabDab(std::unique_ptr<IPhysicsExecutor> executor,
                        ExecutorWriteBatch batch);
  PreparedNativeGrabDab(const PreparedNativeGrabDab &) = delete;
  PreparedNativeGrabDab &operator=(const PreparedNativeGrabDab &) = delete;
  PreparedNativeGrabDab(PreparedNativeGrabDab &&) noexcept = default;
  PreparedNativeGrabDab &operator=(PreparedNativeGrabDab &&) noexcept = default;
  ~PreparedNativeGrabDab();

  const std::vector<GpuProxyChange> &writes() const { return batch_.writes; }
  bool has_solver() const { return executor_ != nullptr; }
  std::uint64_t revision() const { return batch_.revision; }
  void finish_editor_success();
  void cancel() noexcept;

 private:
  std::unique_ptr<IPhysicsExecutor> executor_;
  ExecutorWriteBatch batch_;
};

/* Fixed rest-bond topology lives for the full Blender stroke. Each dab
 * creates a fresh small CPU XPBD island from exactly the currently selected
 * vertices plus a pinned one-ring halo. It reads CURRENT editor positions
 * via a callback, so separate dabs do not revert previous editor writes.
 *
 * IMPORTANT: this per-dab CPU adapter intentionally reallocates a small
 * executor island. It is a correctness/native integration preview, NOT the
 * final persistent GPU executor. It does not modify rest_world or editor data.
 */
PreparedNativeGrabDab prepare_cpu_native_grab_dab(
    const World &rest_world,
    const GpuTopologyBondIndex &topology,
    const std::vector<NativeGrabSample> &samples,
    const std::function<Vec3(std::size_t)> &current_position,
    const NativeGrabDab &dab);

}  // namespace sculpt_physics
