# V15 — Clay, Clay Strips, Smooth, Draw/Nudge/Gravity with local XPBD
**22 November 2026 · PR #24 · Research implementation / OFF by default**

## What was actually implemented

The same `IPhysicsExecutor` used by the experimental native Grab V14
now accepts **fully computed native Blender translations**.

This distinction matters: a Clay or Clay Strips brush should still feel
like a real clay brush. A Smooth brush should use Blender's neighbor
calculation, not a generic placeholder Laplacian average. The V15 CPU
prototype takes those already computed translations and adds a
compliant XPBD bond response as a secondary stage.

| Brush | Blender integration point | Native math retained | Status |
| --- | --- | --- | --- |
| Clay | `mesh/brushes/clay.cc` | Stock plane projection, offset, strength, per-vertex mask/texture/falloff | Native opt-in source written; full Blender build/viewport **not tested** |
| Clay Strips | `mesh/brushes/clay_strips.cc` | Brush-local cube, parabolic Z, trim, frontface, automask, texture, directional offset | Native opt-in source written; full Blender build/viewport **not tested** |
| Smooth | `mesh/brushes/smooth.cc` | Interior neighbor averaging, stock iteration strengths, shape-preserve option | Native opt-in source written; excludes active layer-delta path; full Blender build/viewport **not tested** |
| Draw | `mesh/brushes/draw.cc` | Normal-dependent offset, stock common factors, pressure/strength | Native opt-in source written; full Blender build/viewport **not tested** |
| Nudge | `mesh/brushes/draw.cc` | Stock tangent offset/cross product and strength | Same V15 native path; brush ID retained |
| Gravity | `mesh/brushes/draw.cc` | Stock gravity direction and radius/scale | Same V15 native path; brush ID retained |

**Already verified in automatic tests:** `test_native_other_brushes_executor_adapter.cc`
covers all six kinds, multiple dabs, negative smoothing directions, masked/
hidden pinned particles, changing selected vertices, cancellation, invalid
translations, duplicate IDs and topology-generation errors. It compiles on
Linux GCC and Windows MSVC with no CUDA dependency.

**Not verified:** full Blender CMake with V15 enabled, viewport results,
native Undo/Redo and memory safety during a real callback, true Multires
CCG / sculpt layer writes, Warp GPU execution, performance of large meshes.
The existing CI only builds the standalone physics component. Do not
claim the full Blender native callback is tested from that result.

## Follow-up refinement — 24 November 2026

The existing brushes are the priority; do not add Melt/Infection/other
simulation families until this native path is robust.

- Common CPU XPBD: precompute invariant alpha/denominator/mass coefficients
  once per dab, preserve constraint ordering and skip fixed-fixed halo
  bonds in the iterative solver. Reject overflowing timestep squares.
  This reduces repeated operations but uses additional reusable coefficient
  scratch; measure CPU latency and memory on real organic proxy graphs
  before claiming a speedup.
- V14 Grab: exact zero cursor delta bypasses the solve and native PBVH
  writes. Fully pinned selections bypass even proxy coordinate reads.
  Exclude distant pinned targets from the active workset; include them
  only when adjacent halo constraints require them. Partial mask already
  belongs to the native factor, so use unit mass for unpinned particles
  (as in V15), rather than reweighting the same mask in inverse mass.
- V14/V15: a staged empty write batch is acknowledged without clipping
  zero translations, recalculating PBVH bounds or tagging draw caches.
- Regression tests check stationary and pinned Grab, sparse editor reads,
  stale topology, fixed-fixed bond equivalence and timestep failure
  atomicity. Native callbacks remain OFF by default; full Blender build
  and viewport verification are still required.

Next quality gates: tune Grab stretch resistance and boundary continuity;
preserve Clay/Clay Strips plane and depth profile; make Smooth reduce
high-frequency noise without fighting native shape preservation; compare
actual brush output with stock at matched pressure/mask. Keep per-brush
material controls independent from native strength, and benchmark
first-dab versus continuation. Then move executor ownership to the stroke
and connect a verified native GPU backend.

## Core code

- `experimental/sculpt_physics_cpu/native_grab_executor_adapter.hh/.cc`
  — `NativeSculptBrushKind`, `NativeBrushTranslation`,
  `prepare_cpu_native_brush_dab`.
- `source/blender/editors/sculpt_paint/mesh/brushes/physics_brush_v15.hh`
  — header-only native Sculpt bridge shared between brushes. It caches
  a rest topology/CSR once per stroke, prepares an active+fixed one-ring
  local island, validates all numeric results and native PBVH Undo node
  coverage **before writing anything**.
- `source/blender/editors/sculpt_paint/mesh/sculpt_intern.hh`
  — independent `StrokeCache` V15 state. No .blend DNA changes.
- `source/blender/editors/sculpt_paint/CMakeLists.txt`
  — separate build switch `WITH_SCULPT_XPBD_OTHER_BRUSHES_V15`.
- `experimental/sculpt_physics_cpu/test_native_other_brushes_executor_adapter.cc`
  — cross-brush CPU reference tests.

Each dab currently builds a fresh small CPU XPBD executor, retaining
only the rest topology and CSR cache across the whole stroke. This is a
first **correctness** bridge, not yet an optimized persistent CUDA/CPU
solver. Native Undo remains Blender's responsibility.

## Build / activation on Windows VS2022

Use a **separate experimental build directory**. Add the following
three flags to the full Blender CMake configure command you already use:

```text
-DWITH_SCULPT_XPBD_GRAB_EXPERIMENTAL=ON
-DWITH_SCULPT_XPBD_GRAB_EXECUTOR_V14=ON
-DWITH_SCULPT_XPBD_OTHER_BRUSHES_V15=ON
```

All three are OFF by default. V15 deliberately requires V14's
executor scaffolding. Enabling the flags does **not** activate the
experimental brushes automatically.

In PowerShell before starting the experimental Blender executable:

```powershell
$env:BLENDER_SCULPT_XPBD_OTHER_BRUSHES_V15 = "1"
$env:BLENDER_SCULPT_XPBD_GRAB_EXECUTOR_V14 = "0"
$env:BLENDER_SCULPT_XPBD_GRAB_V12 = "0"
& "C:\\your-experimental-build\\bin\\Release\\blender.exe"
```

Disable V15 to restore the stock brushes:

```powershell
Remove-Item Env:BLENDER_SCULPT_XPBD_OTHER_BRUSHES_V15 -ErrorAction SilentlyContinue
```

The V15 code prints
`[V15 physical brush] CPU XPBD translation preview ACTIVE` when first
used on a supported ordinary PBVH Mesh stroke; otherwise it declines to
the stock brush. If a failure occurs **after** a physical dab, V15
freezes the remainder of that stroke to avoid mixing incompatible
mid-stroke evaluators. Release the mouse and start a fresh stroke.

## Manual tests needed before merging or claiming support

- [ ] Compile full Blender with V15 ON under Visual Studio 2022, solve
      any missing C++ includes/ABI/CMake or mesh API changes.
- [ ] Unset the runtime environment variable and compare stock Clay,
      Clay Strips, Smooth, Draw, Nudge, Gravity against the existing build.
- [ ] Clay: plane orientation, inverted stroke, distance falloff, pressure,
      weak/strong brush strength and the masked boundary.
- [ ] Clay Strips: thin/wide strips, frontface, Z parabolic depth, trim,
      texture alpha, orientation, invert direction, brush cube footprint.
- [ ] Smooth: low/high strength, 1–4 stock smoothing iterations,
      shape-preserve option, mesh boundary and sparse Undo.
- [ ] Draw/Nudge/Gravity: direction, negative strength, radius and repeated
      dabs crossing PBVH nodes.
- [ ] Undo/Redo multiple completed strokes; save/reopen a **copy**;
      compare coordinates and unrelated UV/color/Face Set data.
- [ ] Confirm V15 never acts on Dyntopo, Multires grids, active Sculpt
      Layer delta paths or a symmetry/shape-key/modifier configuration.
- [ ] Stress at 10k, 100k, 1M vertices with profiler p50/p95 for
      topology startup, per-dab target build, XPBD solve, native PBVH
      writeback and viewport draw. No CUDA speedup claims.
- [ ] Inject failures before editor writeback and ensure no partial mesh
      edits; validate mirrored/axis-locked movement is declined.
- [ ] Confirm all V15 data is discarded with the end of the stroke.

## Known limitations and priority fixes

1. **Full Blender native compile and viewport testing is still missing.**
   The standalone CTest passes do not compile the guarded V15 branch
   because the guarded source belongs to the large Blender target.
2. **Editor writeback is not yet fully failure-atomic across PBVH nodes.**
   We validate first, but Blender's native rollback/journal must become
   the authority on an unexpected mid-write editor failure.
3. **Smooth Sculpt Layer isolation is not implemented.** The V15 hook
   declines the existing layer-delta path, rather than writing incorrect
   base mesh positions. Clay Strips likewise retains the layer-detail
   path when active.
4. **Local pinned one-ring** is an intentionally approximate material
   boundary model, not global elastic/physical sculpting.
5. **Per-dab executor allocations** and rebuilding target lists must
   be replaced with per-stroke persistent local buffers and dynamic
   active/halo updates before real-world GPU acceleration.
6. **Compliance** is a fixed research constant `1e-6`. Define
   per-material sliders/presets and CPU↔Warp parity gates later.
7. **CUDA is not connected to the native bridge.** Warp kernels compiled
   on the Warp CPU JIT in CI; no end-to-end NVIDIA device or Blender
   GPU benchmark has been run.
8. Keep the normal Sculpt brush path unchanged when runtime OFF and
   avoid any new default material preset that silently enables physics.

**Codex next handoff:** Full Windows V15 native compile → capture
screenshots/logs/Undo results → fix runtime blockers → persistent executor
per stroke → safe native writer → GPU optional backend → Multires/Sculpt
Layers and precision benchmarks. Do not merge Draft PR #24 into main.
