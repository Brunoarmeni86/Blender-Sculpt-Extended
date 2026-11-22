/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "gpu_workset.hh"
#include "solver_policy.hh"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sculpt_physics {

/* An adapter contract shared by future native CPU/CUDA/Vulkan executors.
 *
 * The executor only OWNS AN ISOLATED PHYSICS PROXY. It must NEVER write
 * directly into Blender's PBVH/CCG/attributes or manage native Undo.
 * Blender remains the authority for mask, factors, attributes, topology
 * generations, Sculpt Layers, transactions and actual editor writeback.
 */
struct ExecutorCapabilities {
  SolverDevice device = SolverDevice::CPU;
  SolverFamily family = SolverFamily::XPBDSurface;
  bool has_real_executor = false;
  bool supports_compliant_targets = false;
};

struct ExecutorTarget {
  std::size_t global_index = 0;
  Vec3 goal{};
  double compliance = 0.0;
};

struct ExecutorEdit {
  std::size_t global_index = 0;
  Vec3 position{};
};

/* Incremental editor-writeback handshake. A staged batch does not mutate the
 * executor: acknowledge only AFTER native PBVH/CCG writeback succeeded.
 * An out-of-date revision can never acknowledge a newer simulation state. */
struct ExecutorWriteBatch {
  std::uint64_t revision = 0;
  std::vector<GpuProxyChange> writes;
};

class IPhysicsExecutor {
 public:
  virtual ~IPhysicsExecutor() = default;

  virtual ExecutorCapabilities capabilities() const = 0;

  /* Allocating a new island is allowed ONLY between strokes. Passing a
   * truncated graph is refused in the safe initial reference.
   * Native adapter owns the World and invalidates the island when the
   * topology generation / Multires anchors change. */
  virtual void allocate_island(const GpuLocalIsland &island) = 0;
  virtual std::uint64_t topology_generation() const = 0;
  virtual bool stroke_active() const = 0;

  /* Snapshot the committed proxy state once per entire stroke. */
  virtual void begin_stroke() = 0;

  /* Sparse edits and targets use GLOBAL proxy IDs, never island offsets.
   * Invalid input is rejected BEFORE any state mutation.
   * update_dirty must never move pinned/non-Undo-covered vertices. */
  virtual void upload_dirty(const std::vector<ExecutorEdit> &edits) = 0;
  virtual void set_targets(const std::vector<ExecutorTarget> &targets) = 0;

  /* Quasistatic XPBD baseline; no collisions, gravity, plasticity,
   * velocity integration, Multires or Blender Undo. All-or-nothing step.
   * Empty target sets are a genuine no-op, like the native local World. */
  virtual void step(double dt, int substeps, int iterations) = 0;

  /* Read all CHANGED and writable positions relative to the committed
   * beginning of this stroke. Host/editor must then validate its own
   * PBVH/CCG transaction before calling commit(). */
  virtual std::vector<GpuProxyChange> download_dirty(double epsilon = 1e-12) const = 0;

  /* Only changes since the last successfully acknowledged editor writeback.
   * Earlier full-stroke baseline remains intact for cancellation.
   * Initial reference checks all local particles; a generation-stamped dirty
   * set is a later optimization. */
  virtual ExecutorWriteBatch stage_incremental(double epsilon = 1e-12) const = 0;
  virtual void acknowledge_incremental(std::uint64_t revision) = 0;

  /* Commit only the ISOLATED proxy state; it does not write Blender geometry.
   * Cancel reverts all dabs to the snapshot taken by begin_stroke(). */
  virtual void commit() = 0;
  virtual void cancel() = 0;
};

/* First real C++ implementation of the common contract.
 * This is an ORIGINAL stand-alone CPU numerical reference, NOT a bridge to
 * CUDA yet. It implements same compliant target and distance-bond XPBD
 * equations as World, but NOT its velocity/fracture/plasticity/full-scene
 * features. Future GPU backends must pass the same interface tests. */
std::unique_ptr<IPhysicsExecutor> make_cpu_xpbd_executor();

/* Fail closed: non-CPU devices are not registered simply because Warp's
 * Python kernel compiled on CPU or a CUDA device is installed. */
std::unique_ptr<IPhysicsExecutor> make_native_executor(SolverDevice device);

/* A solver plan also contains its physical solver FAMILY. A CPU device is
 * not evidence of a DEM/volume/MPM implementation: reject unsupported
 * families instead of silently substituting XPBD surface. */
std::unique_ptr<IPhysicsExecutor> make_native_executor(const SolverPlan &plan);

}  // namespace sculpt_physics
