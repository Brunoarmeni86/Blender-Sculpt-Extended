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

/* Material response independent of the native brush strength. The bond
 * scale multiplies compliance (higher = softer); native_rest_follow lets
 * rest lengths approach the finished native target geometry for this dab,
 * rather than elastically restoring all details of the stroke-start mesh.
 * This is a local edge response, not a volume or plasticity solver. */
struct NativeMaterialResponse {
  double bond_compliance_scale = 1.0;
  double native_rest_follow = 0.0;
};

struct NativeGrabDab {
  Vec3 delta{}; // incremental Grab displacement in mesh/object space
  double compliance = 1.0e-6;
  double dt = 1.0 / 60.0;
  int substeps = 2;
  int iterations = 5;
  std::uint64_t topology_generation = 0;
  NativeMaterialResponse material{};
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

/* V15 common path for sculpt brushes which already calculated their
 * EXACT native deformation, including falloff, strength, pressure, masking,
 * texture, silhouette or specialized smooth/clay-plane geometry.
 * The value is a finished translation, not a raw direction or strength.
 * Never reapply factor/strength inside the XPBD adapter. */
enum class NativeSculptBrushKind { Clay, ClayStrips, Smooth, Draw, Nudge, Gravity };

struct NativeBrushProfile {
  double target_compliance = 1.0e-6;
  NativeMaterialResponse material{};
};

/* Opt-in starting points for artist trials; not calibrated material
 * constants. Legacy settings remain unchanged unless explicitly selected. */
NativeBrushProfile experimental_native_brush_profile(NativeSculptBrushKind kind);

struct NativeBrushTranslation {
  std::size_t global_index = 0;
  Vec3 translation{}; // fully factored original Blender displacement
};

struct NativeBrushDab {
  NativeSculptBrushKind kind = NativeSculptBrushKind::Clay;
  double compliance = 1.0e-6;
  double dt = 1.0 / 60.0;
  int substeps = 2;
  int iterations = 5;
  std::uint64_t topology_generation = 0;
  // Blend ONLY the physics correction into the finished native translation.
  // 0 = unmodified native target (editor should bypass the adapter entirely);
  // 1 = original fully physical V15 response. Independent of brush strength.
  double assist = 1.0;
  NativeMaterialResponse material{};
  bool colored_order = false;
  // V16-B material preview, independently opt-in. Coordinate-only.
  bool clay_material = false;
  double clay_yield = 0.035;
  double clay_plasticity = 0.65;
  double clay_spread = 0.55;
  // 0=V2 reference, 1=deposit, 2=compress, 3=flow.
  int clay_behavior = 0;
  Vec3 clay_center{};
  double clay_radius = 0.0; // object-space brush footprint
  Vec3 clay_stroke_delta{};

};

using PreparedNativeBrushDab = PreparedNativeGrabDab;

/* Isolated numerical solve for the selected unique IDs, + pinned one-ring.
 * Strictly validates samples and topology, performs no editor writes.
 * Input translations MUST be fully computed by the original native brush.
 * For smoothing, the caller supplies its ORIGINAL smoothed displacement,
 * not the uniform brush_targets Smooth approximation.
 *
 * An empty/zero set is a no-op. Invalid, duplicate and pinned samples are
 * rejected or skipped safely, with no editor change. */
PreparedNativeBrushDab prepare_cpu_native_brush_dab(
    const World &rest_world,
    const GpuTopologyBondIndex &topology,
    const std::vector<NativeBrushTranslation> &samples,
    const std::function<Vec3(std::size_t)> &current_position,
    const NativeBrushDab &dab);

/* Solve an already packed island, without reconstructing a World or CSR.
 * Samples refer to island global_ids. All writable, mobile vertices must
 * have exactly one nonzero native target; all other vertices are pinned. */
PreparedNativeBrushDab prepare_cpu_native_brush_island(
    GpuLocalIsland island,
    const std::vector<NativeBrushTranslation> &samples,
    const NativeBrushDab &dab);

}  // namespace sculpt_physics
