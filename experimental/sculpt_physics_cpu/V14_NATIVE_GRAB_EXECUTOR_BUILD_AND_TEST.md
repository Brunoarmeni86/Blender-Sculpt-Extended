# Blender Sculpt Extended — Native Grab V14 integration preview
**22 November 2026 · PR #24 · Experimental / Default OFF**

## What is actually connected

This branch has an **opt-in native Blender source path** for ordinary
PBVH **Mesh** Grab using the `IPhysicsExecutor` CPU XPBD reference.

| Component | Source | Purpose |
| --- | --- | --- |
| Native Blender callback | `source/blender/editors/sculpt_paint/mesh/brushes/grab.cc` | Receives existing native brush mask, falloff, pressure/silhouette factors, and uses the standard Grab node mask for Sculpt Undo coverage |
| Separate per-stroke state | `source/blender/editors/sculpt_paint/mesh/sculpt_intern.hh` | Type-erased V14 rest topology and previous cumulative delta, independently from legacy V12/V13 |
| Native build switch | `source/blender/editors/sculpt_paint/CMakeLists.txt` | Compiles adapter/executor sources only when V14 build opt-in is enabled |
| Reusable rest topology | `gpu_workset.hh/.cc` | Caches adjacency CSR once per stroke, expands an active set by one pinned ring, verifies fixed-boundary bond crossings |
| Pure CPU per-dab adapter | `native_grab_executor_adapter.hh/.cc` | Refreshes current editor positions, builds a small local island, applies compliant XPBD and prepares global-ID writes |
| Numerical executor | `physics_executor.hh/.cc` | Target/bond solver with strong pre-write staging, revision ACK and explicit commit/cancel |
| Unit tests | `test_native_grab_executor_adapter.cc`, `test_gpu_workset.cc`, `test_physics_executor.cc` | Two consecutive dabs, pinned boundary, mask, stale topology, invalid input, no editor mutation until commit |
| Cross-language tests | `emit_xpbd_reference.cc`, `gpu/test_warp_xpbd_compliant.py` | Actual C++ World ↔ Warp CPU JIT compliant XPBD parity on a controlled graph |

**Critical distinction:** the V14 *Blender source callback* is now
written but has **NOT** undergone a full Blender VS2022/native UI compile
and real stroke interaction test. The standalone executor/adapter CTest
is a separate verified component; don't claim V14 runs in the viewport
until a full native build and manual tests confirm it.

V14 currently **creates a new small CPU executor every dab**, while
preserving the rest-topology/CSR cache for the entire Blender stroke.
This is a correctness-first bridge, **not yet the desired persistent
CUDA executor or optimized native end-to-end brush implementation**.

## Opt-in build on the Windows VS2022 development workstation

**Do not apply these flags to your stable Blender executable.**
Create a separate build directory from the experimental Git branch.
Configure Blender as you normally do on your Windows machine, and add:

```text
-DWITH_SCULPT_XPBD_GRAB_EXPERIMENTAL=ON
-DWITH_SCULPT_XPBD_GRAB_EXECUTOR_V14=ON
```

These are CMake options: both must be ON to compile the V14 callback.
The second option is explicitly OFF by default and errors if the
first build scaffold is OFF. Keep whatever paths/compiler/dependencies
your Blender build already requires; these two flags alone do not
constitute a full Blender build command.

Start the resulting **experimental** Blender executable with this
environment variable, for example in PowerShell:

```powershell
$env:BLENDER_SCULPT_XPBD_GRAB_EXECUTOR_V14 = "1"
$env:BLENDER_SCULPT_XPBD_GRAB_V12 = "0"  # Avoid confusing the legacy preview
& "C:\\your-experimental-build\\bin\\Release\\blender.exe"
```

Unset it to return to the stock Grab, without recompilation:

```powershell
Remove-Item Env:BLENDER_SCULPT_XPBD_GRAB_EXECUTOR_V14 -ErrorAction SilentlyContinue
```

When activated on an eligible normal Mesh, Blender prints
`[V14 XPBD Grab] native IPhysicsExecutor preview ACTIVE` once per
stroke. If validation fails before editor writeback, it prints
`[V14 XPBD Grab] disabled: ...`.

If V14 has already written earlier dabs in the same stroke, a later
failure **freezes the rest of that stroke** instead of switching to
stock Grab's incompatible cumulative displacement convention. Finish
the stroke and start a new one. If V14 never activated, ineligible
cases fall back to stock Grab.

## Initial manual acceptance checklist

- [ ] Build with both opt-in flags ON using VS2022. No unresolved symbols,
  include failures or unexpected dependency on Warp/Python/CUDA.
- [ ] Runtime variable missing: regular Grab stays visually identical.
- [ ] Enable V14: ordinary unmodified Mesh, one Grab stroke with several
  dabs, console confirmation, repeat with varied falloff/pressure.
- [ ] Inspect vertex positions and silhouette on a simple connected quad
  surface after moving the cursor across several PBVH nodes. No tears or
  unexplained freezing at one-ring halo.
- [ ] Hide/mask a region. Fully masked vertices must not move; partial
  factors must still produce appropriately scaled goals.
- [ ] Try X/Y/Z axis locks and mirror-clip. V14 should decline modified
  staging and use the safe fail/freeze behavior.
- [ ] Undo one completed native stroke, Redo, and save/reopen a COPY of the
  scene. No corruption of vertex positions or unrelated attributes.
- [ ] Try a Multires object, Dyntopo and active symmetry. V14 must not
  run; stock brush must remain available.
- [ ] With a Sculpt Layer, UV and Polypaint attributes present, confirm
  V14 does not unexpectedly modify them. **This does not mean V14 supports
  Multires Sculpt Layer delta propagation or UV advection.**
- [ ] Compare CPU viewport input→draw p50/p95 against stock Grab on
  10k/100k/1M meshes, and record first-dab initialization separately.

For any test failure include the mesh type, size, screenshot, log,
triggering brush settings, build SHA, and whether the failure occurred
on first dab or after a previously successful V14 dab.

## Correctness scope / research caveats

1. The native callback uses Blender's existing `calc_factors_common...`
   and `clip_and_lock_translations`; it stages all translations before
   writing any PBVH Mesh node. A clipped/locked result that differs from
   the solver's intended change is rejected *before* editing.
2. The adapter uses local **one-ring pinned boundary** constraints. This
   is a quasistatic approximation, not a simulation of the globally
   deforming mesh. Bonds crossing an *included free* vertex still fail
   strict validation.
3. Each dab obtains **current** mesh vertex positions, preserving
   displacements from previous dabs. The rest lengths and topology are
   cached once per stroke; topological edits during the stroke are
   unsupported. V14 verifies vertex/edge counts but cannot detect
   all same-count edge reorderings; disallow any remesh mid-stroke.
4. Native Blender Sculpt must still provide its own Undo coverage,
   journal and rollback. The reference executor's `cancel()` only
   discards isolated proxy state. It cannot reverse PBVH writes that
   Blender may already have applied.
5. Native Blender mesh writeback currently has no full multi-node
   failure/rollback transaction implemented in V14. This is an opt-in
   preview requiring full-source compile, end-to-end Undo testing,
   numerical checks and a separate final release gate.
6. Persistent GPU device buffers, CUDA Streams/Graphs, backend failover,
   granular/MPM/volumetric solvers, actual CCG grid writes and
   deformable Multires Sculpt Layers all remain future work.

## Codex implementation priorities

**P0:** compile V14 on the full Windows Blender VS2022 branch and resolve
all build/runtime issues; test stock Grab bypass, native Undo across
multiple PBVH nodes and axis-lock/mirror rejection.

**P1:** move the isolated `IPhysicsExecutor` lifetime from one dab
to the full stroke. Add a generation-aware `update_island` API that
can expand/shrink the dynamic writable region while preserving rest,
velocity and Sculpt Layer mappings; never allocate an entirely new
CPU/GPU solver per dab.

**P1:** replace temporary full-node translation buffers with native
dirty-node writeback and a transaction journal that can revert
a partially-applied PBVH batch on any failure. Use the existing
Blender Sculpt Undo integration rather than custom parallel Undo.

**P2:** implement the same `IPhysicsExecutor` contract in a compiled
CUDA backend with persistent local buffers; do not require Python
invocation in the brush callback. Measure full viewport latency and
choose CPU automatically when GPU is slower.

**P2:** implement a **real** Multires CCG dirty-grid writeback with
stable per-corner UV and color provenance and active Sculpt Layer deltas.
Validate on several subdivision levels and with symmetry/topological
symmetry on posed meshes.

**P3:** enable DEM, MPM or volume solvers *only after* they implement
their declared physics family and pass CPU↔GPU parity plus native
Undo/Multires tests.

**Merge gate:** keep PR #24 in Draft, leave the stable build unchanged.
