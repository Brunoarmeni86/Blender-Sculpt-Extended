/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "brush_targets.hh"
#include "granular_contacts.hh"
#include "multires_dirty_map.hh"
#include "solver_policy.hh"

#include <cstddef>
#include <memory>
#include <vector>

namespace sculpt_physics {

/* One small CPU end-to-end reference path for Codex to integrate into Blender.
 * It owns no Mesh/PBVH/CCG data and performs NO native Undo or editor writeback.
 * World remains the single authoritative particle/bond state. */
struct CpuStrokeOptions {
  double dt = 1.0 / 60.0;
  int substeps = 2;
  int iterations = 5;
  int bond_hops = 0; // Conservative: no writes outside selected brush nodes.
  GranularContactSettings granular{};
};

struct CpuDabInput {
  AssistedDab brush{};
  BrushPhysicsSettings physics{};
  std::vector<BrushVertexInput> selection; // Native mask/falloff already applied.
  std::vector<std::size_t> granular_candidates; // Broadphase halo, for DEM only.
  std::vector<std::size_t> undo_covered_vertices; // Must cover ALL solver writes.
  bool rebuild_adjacency = false; // Caller signals topology/fracture changes.
};

struct ProxyWrite {
  std::size_t particle_index = 0;
  Vec3 position{};
};

struct CpuDabResult {
  SolverPlan plan{};
  bool used_solver = false;
  std::vector<ProxyWrite> writes; // Staged: editor must validate/commit atomically.
  std::vector<std::size_t> dirty_multires_samples;
  std::vector<std::size_t> dirty_multires_tiles;
};

/* No GPU/volume/MPM pretense: those families require separate, real backends.
 * If a step fails, the ENTIRE standalone stroke is canceled and the runner
 * becomes inactive. A native Blender adapter must also roll back any previous
 * editor writes via its own Undo transaction.
 *
 * Lifetime: caller owns World and must keep topology stable during a stroke.
 * For dynamic bond/fracture changes request rebuild_adjacency on the next dab.
 * The surface correspondence can be reconfigured only between strokes. */
class CpuStrokePipeline {
 public:
  explicit CpuStrokePipeline(World &world) : world_(world) {}
  CpuStrokePipeline(const CpuStrokePipeline &) = delete;
  CpuStrokePipeline &operator=(const CpuStrokePipeline &) = delete;
  ~CpuStrokePipeline();

  void configure_multires(const std::vector<SurfaceAnchor> &anchors,
                          std::size_t samples_per_tile);
  void clear_multires();
  void begin(const SolverRequest &request,
             const AvailablePhysics &available = {},
             const CpuStrokeOptions &options = {});
  CpuDabResult dab(const CpuDabInput &input);
  void commit();
  void cancel();
  bool active() const { return active_; }
  SolverPlan plan() const { return plan_; }

 private:
  World &world_;
  std::unique_ptr<BrushAdjacency> adjacency_;
  std::unique_ptr<MultiresDirtyMap> multires_;
  std::size_t samples_per_tile_ = 0;
  CpuStrokeOptions options_{};
  SolverPlan plan_{};
  bool active_ = false;
};

}  // namespace sculpt_physics
