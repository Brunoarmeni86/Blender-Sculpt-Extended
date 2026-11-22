# IPhysicsExecutor — native backend contract for Codex
**22 November 2026 — PR #24 (experimental, not Blender viewport-ready)**

## Why this exists
The original `CpuStrokePipeline` already owns a complete local
`World` XPBD/DEM stroke and its standalone rollback. The Python/Warp
`LocalXpbdSession` and `ResidentContactSession` are **separate** proof
of concepts. Neither can be called from Blender C++ today without a
production bridge.

This step introduces `physics_executor.hh/.cc` as a **precise executor
contract** for one local, preselected physics island:
a CPU implementation is present **now**, and an NVIDIA CUDA/Vulkan
implementation must be registered only after its native compiled code
and numerical tests succeed.

## Code navigation

| File | Responsibility |
| --- | --- |
| `physics_executor.hh` | Public abstract `IPhysicsExecutor`, typed targets and edits, advertised capabilities |
| `physics_executor.cc` | Working local `CpuXpbdExecutor` with compliant target + bond XPBD and transactional isolated state |
| `test_physics_executor.cc` | Backend refusal, invalid edits, compliant XPBD parity, cancel/commit, fixed halo |
| `gpu_workset.hh/.cc` | Stable global↔local IDs, CSR graph, pinned halo, dirty writeback validation |
| `sculpt_physics_pipeline.hh/.cc` | Existing separate CPU World brush/DEM runner (DO NOT replace or disable) |
| `gpu/warp_xpbd_local.py` | Python/Warp CPU/CUDA research kernel; not a native executor |
| `gpu/GPU_OPTIMIZATION_CODEX.md` | Previous real-Warp CPU tests and GPU optimization proposal |

## Exact stroke protocol

```cpp
auto executor = make_native_executor(SolverDevice::CPU);
// Future CUDA executor: make_native_executor(WarpCUDA) must still fail
// until its verified native implementation is actually registered.

const auto island = cached_index.build(world, active_ids, halo_ids,
                                        undo_covered_ids, generation, false);
executor->allocate_island(island);
executor->begin_stroke();

// For each physics dab in this Blender-native stroke:
executor->upload_dirty(external_undo_covered_edits); // optional
executor->set_targets({{global_proxy_vertex_id, goal, compliance}});
executor->step(dt, substeps, iterations);
auto all_deltas = executor->download_dirty(); // staged; no editor write yet

// Blender adapter must check generation, masks, Undo node coverage,
// Multires CCG and active Sculpt Layers; stage and apply atomically.
if (native_editor_transaction_successful) {
  executor->commit();  // commits ONLY internal proxy cache state
}
else {
  executor->cancel();  // Blender editor must ALSO revert its own writes!
}
```

**Do not call `commit()` after each dab** when one native Undo transaction
covers the entire stroke. Multiple `step()` calls work on the evolving
internal positions; `download_dirty()` currently returns **all changes
relative to the beginning of the stroke**, not just the most recent dab.
This is intentional for safe full-stroke rollback. Codex may later add
a dirty generation cursor and acknowledged per-dab deltas for performance.

### Verified-plan factory and stronger parity gate

Use `make_native_executor(const SolverPlan &)` when routing a
`choose_solver` decision. Unlike a device-only request, this checked
factory requires **both** `SolverDevice::CPU` and
`SolverFamily::XPBDSurface` plus `implementation_available=true`.
It rejects DEM, MPM, XPBDVolume, CUDA and Vulkan: no fallback is silently
substituted for a different physical material.

`test_physics_executor.cc` now compares the CPU executor to the native
`World::step_local()` in two cases: a compliant single target **and**
an actual connected distance bond crossing a fixed non-Undo halo boundary.
Both tests use one substep to compare the same quasistatic equations,
not additional unimplemented dynamics or long-term plastic evolution.

## Explicit backend boundaries

- `make_native_executor(CPU)` returns a real C++ local XPBD executor;
  `make_native_executor(WarpCUDA/VulkanCompute)` **throws**.
  Python Warp CPU test success does not register any GPU executor.
- This local CPU reference only solves position XPBD target constraints
  and distance bonds; it intentionally omits `World` velocity, gravity,
  plasticity, dynamic fracture, collision and granular contact history.
- `World` and Blender Mesh/PBVH/CCG are **never** mutated by this new
  executor. A future `BlenderSculptPhysicsAdapter` should own both editor
  Undo and a `World` or compatible physics proxy.
- `allocate_island` rejects truncated physical graphs (crossing bonds).
  Missing halo vertices must be expanded before solving in strict mode.
- Input targets/edits are global proxy IDs; duplicate, missing, fixed,
  NaN or Undo-excluded IDs are rejected without partial state updates.
- Each `step` computes a copy, applies all constraints and publishes
  it only after final finite validation. The compliant XPBD equations
  match the native CPU `World`'s per-constraint projection.
  Numerical equality is tested on an isolated particle with gravity
  disabled; no claim of full `World` dynamics parity is made.
- `cancel` reverts **all** dabs since `begin_stroke`; `commit` advances
  the proxy's baseline only after the editor transaction succeeds.
- Stable topology generation must be validated by the caller before
  each step and each editor write. The reference owns an island snapshot,
  not live access to Blender's changing graph.

## Proposed next implementation gates

**Gate 1 — Native Sculpt adapter, CPU:** Refactor existing Grab V13 callback
to construct `GpuLocalIsland` from PBVH-selected and halo vertices and to
pass exact Blender mask/falloff/automasking data into compliant targets.
Stock Standard sculpt bypass must remain byte-identical. Use native Undo
and cancel, not the proxy state alone.

**Gate 2 — Numerical equivalence:** Expand tests from one particle to
heterogeneous bonds, several connected organic meshes, multiple dabs,
partial strength and active/pinned halo. Include acceleration,
velocity, plasticity and bond breakage only if the executor actually
implements them. Never weaken tolerances to hide a missing feature.

**Gate 3 — Native CUDA executor:** Add optional `WarpCudaExecutor`
implementing the same interface with Warp **AOT / C++ callable CUDA**
kernels, persistent device arrays and topology-gen cache invalidation.
No Warp Python calls per dab. Run on a real NVIDIA GPU and compare
full writeback positions against this CPU oracle and the existing
`World` where their physical feature sets match.

**Gate 4 — GPU performance:** Reuse pinned host staging and async CUDA
buffers; transfer only changed proxy vertices/targets and read back only
the editor-required dirty subset. Cache CUDA graphs if graph topology
and memory addresses remain stable. Profile p50/p95 of full Blender
input → PBVH → solver → CCG/Undo write → viewport, not kernel-only time.

**Gate 5 — Multires and materials:** Implement real CCG grid/sample IDs,
active Sculpt Layer deformation, UV/polypaint and per-corner attributes;
then implement volume XPBD, granular friction and MPM as distinct solver
families. Expose backend capabilities **per family**, not per GPU alone.

## Acceptance criteria
- Linux GCC/Clang and Windows MSVC CTest pass.
- `test_physics_executor.cc` tests staged writeback, invalid
  duplicate targets, no partial edits, full-stroke rollback, commit
  baseline and numerical target compliance vs `World`.
- No external dependency (CUDA/Warp/Python) is required for C++ core build.
- `main` remains untouched and this PR remains a Draft.
- Blender VS2022 full build/viewport Undo/Multires CUDA checks are still TODO.

**Important:** This is a deliberately small implementation of a new
**island solver contract**, not yet a replacement of `CpuStrokePipeline`.
Do not confuse the two transaction owners when Codex integrates them.
