# START HERE — Sculpt Physics CPU → Blender / Multires / GPU

**Status: experimental reference (PR #24), 22 November 2026**

This is the **single starting point for Codex**. The aim is a maintainable,
single physics architecture for *existing* Blender Sculpt brushes rather than
unrelated brush-specific solvers.

**New native executor boundary:** [`PHYSICS_EXECUTOR_CODEX.md`](PHYSICS_EXECUTOR_CODEX.md)
defines the first actual `IPhysicsExecutor` C++ interface and a working
`CpuXpbdExecutor` reference for **compliant XPBD target/bond constraints**.
It now owns a complete isolated stroke transaction and refuses GPU devices
until a verified native executor is linked. This is the required contract
for a future compiled Warp CUDA bridge; it does not yet write to Blender.

**GPU optimization update:** There are now persistent local CUDA/Warp
prototype executors for XPBD bonds and DEM contacts. Read
[`gpu/GPU_OPTIMIZATION_CODEX.md`](gpu/GPU_OPTIMIZATION_CODEX.md)
before changing GPU code. They keep local proxy arrays on-device across dabs
and use sparse readback; neither is attached to Blender's C++ editor pipeline.

**Current quality/performance additions:** `GpuTopologyBondIndex` now
uses a reusable CSR index to avoid a global bond scan for each GPU island;
`pack_gpu_island_relative` protects small sculpt displacements far from
object origin when using float32 CUDA; `LocalXpbdSession` now accepts
authoritative C++ rest lengths and holds hard brush targets after each
color pass. Benchmarks and validation are listed in
[`gpu/GPU_OPTIMIZATION_CODEX.md`](gpu/GPU_OPTIMIZATION_CODEX.md).

**Important reality check:** The C++ reference can execute CPU XPBD / approximate
DEM strokes and produce validated proxy-vertex changes. It does **not** yet write
to Blender's PBVH, Multires CCG grids, Sculpt Layers or native Undo. Volume XPBD
is a separate tested reference; MPM, Vulkan and CUDA editor executors are NOT
implemented. The isolated Python/Warp XPBD and DEM kernels now compile
and execute on Warp's CPU backend in CI (Warp 1.19.0), with oracle parity.
**NVIDIA CUDA kernel execution and Blender integration remain unvalidated.**

## 1. Which code belongs where?

| File | Responsibility | Current readiness |
| --- | --- | --- |
| `physics_core.hh/.cc`, `adaptive_solver.cc` | Authoritative XPBD CPU `World`, bond state, local solve, rollback | Standalone C++ |
| `brush_targets.hh/.cc` | Turn Blender-provided mask/falloff/pressure into sparse goals | Standalone C++ |
| `granular_contacts.hh/.cc` | Sparse cell-hash overlap/cohesion goals for small DEM-like particles | Approximate CPU |
| `volume_constraints.hh/.cc` | Independent tetra XPBD volume conservation on a volumetric proxy | Standalone CPU, not wired |
| `solver_policy.hh/.cc` | Choose material solver and verified device/family capabilities | Planning only; no executor loading |
| `sculpt_physics_pipeline.hh/.cc` | **New single CPU execution path**: begin, plan, solve, stage, cancel/commit | Integrated with CPU World only |
| `sculpt_surface_bridge.hh/.cc` | Preserve regular-mesh or anchored Multires detail on physical proxy deformation | Standalone reference |
| `multires_dirty_map.hh/.cc` | Map dirty proxy particles to dependent evaluated surface samples | Standalone index; not actual CCG tiles |
| `gpu_workset.hh/.cc` | Build a compact CPU→GPU local proxy island, pinned boundary, validated sparse writeback | Standalone C++ contract, not CUDA execution |
| `gpu/warp_resident_contact.py` | Reuse on-device granular state/HashGrid across multiple dabs | Python/Warp CUDA experiment; no native editor |
| `gpu/warp_xpbd_local.py` | Edge-colored parallel XPBD local proxy constraints, resident arrays | Python/Warp CUDA experiment; no native editor |
| `gpu/bench_warp_backend.py` | Runtime and transfer measurements p50/p95 | Benchmark script; no results claimed |
| `gpu/warp_granular_probe.py` | CUDA/HashGrid experiment with CPU NumPy oracle | Not native C++ or viewport |
| `test_sculpt_physics_pipeline.cc` | End-to-end CPU lifecycle, Undo-coverage and failure rollback regression | CTest / GitHub Actions |

**Never copy an upstream solver wholesale into this fork.** Audited
repositories, licenses and proposed families are described in
[`MULTISOLVER_SOURCE_REUSE_AND_INTEGRATION_20261122.md`](../../doc/sculpt_extended/MULTISOLVER_SOURCE_REUSE_AND_INTEGRATION_20261122.md).

## 2. Data flow — one dab

```text
Blender's existing Sculpt brush (no new mode)
    |
    v
Native PBVH / mask / falloff / symmetry and Undo-node selection  [TODO]
    |
    v
CpuStrokePipeline::dab(CpuDabInput)
    |
    +-> plan_assisted_brush()                [CPU C++ EXISTS]
    +-> plan_granular_contacts()             [CPU C++ EXISTS, DEM approximation]
    +-> World::set_brush_targets()
    +-> World::step_local()                   [CPU C++ EXISTS]
    +-> validate every dirty particle is covered by native Undo
    |
    v
CpuDabResult: sparse proxy writes, affected Multires samples
    |
    v
Blender transaction writer / active Sculpt Layer / PBVH/CCG   [TODO]
    |
    v
Native Undo commit, cancellation, redraw                       [TODO]
```

`CpuStrokePipeline` deliberately rejects a proposed GPU/volume/MPM backend:
selecting a backend in `solver_policy` **does not implement it**.

## 3. The CPU integration contract

```cpp
World world; // caller builds a suitable coarse proxy and bonds
CpuStrokePipeline pipeline(world);
pipeline.begin(request, available, options);
CpuDabResult report = pipeline.dab(dab);
if (/* all Blender output nodes validated and written */) {
  pipeline.commit();
}
else {
  pipeline.cancel();  // ALSO undo Blender's native geometry writes!
}
```

- Standard / PhysicalAssist influence=0: skip solving and return the
  untouched native brush output. Do not spawn a physics World for Standard.
- `selection` contains unique proxy IDs and the **final** Blender brush
  influence; all mask, pressure, texture and symmetry decisions remain in Blender.
- `undo_covered_vertices` must include every particle that XPBD can move.
  `bond_hops=0` is the conservative default; larger halos need corresponding
  native Undo/PBVH node coverage.
- Invalid input, solver failure or invalid output triggers whole **standalone**
  stroke cancellation. Native Blender must roll back prior writeback too.
- `rebuild_adjacency` is required after graph changes/fracture. Use a
  topology generation ID to automate it before real editor integration.
- `CpuDabResult::writes` are only proxy staging data; they do not preserve
  UV/CCG by themselves. Do not directly assign the entire evaluated mesh.
- For DEM the contact-goal + artistic-drag merge is approximate; friction,
  velocity history, robust contact and material coupling are not complete.

## 4. Real implementation priorities, in order

| Gate | Deliverable | Acceptance test |
| --- | --- | --- |
| **P0 / CPU contract** | This pipeline compiles and passes all existing CTest suites, including injected invalid input, cancellation, sparse writes and masks | Linux CI + Windows MSVC + ASan/UBSan |
| **P1 / Normal Sculpt** | Wire **Grab and Clay** through existing native PBVH callbacks behind opt-in flag | Standard mode identical; Undo/Redo and masks; 100 sequential strokes |
| **P2 / Multires** | Build stable coarse anchors, real CCG dirty grid map, active Sculpt Layer delta writer, independent layer/UV/color validation | 2+ layers, 3+ Multires levels, seams, symmetry, Undo/save/reload |
| **P3 / GPU backend** | Register real per-family executors (CUDA + eventual Vulkan) with persistent buffers; support CPU fallback | Parity + p50/p95 end-to-end; skip GPU for small dabs |
| **P4 / Materials** | Bind tetra XPBD to a real volume cage; implement DEM friction/contact history, MLS-MPM for mud, then reconstruction | Material fidelity, long strokes, mass/volume and UV provenance |

Only one gate should become default-enabled after the previous gate is measured.

## 5. Known problems and specific improvements to propose

1. **Topology and memory:** current `BrushAdjacency` is a vector of vectors.
   Replace with CSR/offset adjacency cached by topology generation. For large
   dense meshes use a **coarse physical proxy for normal Sculpt too**, not
   one particle per vertex.
2. **Latency:** `adaptive_solver.cc` has full-size temporary graph state
   paths; use visit generation stamps and persistent scratch. Benchmark
   selection + solve + Undo + CCG writeback + redraw (not kernel time alone).
3. **Material fidelity:** Clay needs a volume cage / compliant tetra strain
   constraints and shape preservation, not just outward vertex targets.
4. **Smoothing:** current Smooth is uniform neighbor averaging. Add feature,
   curvature and volume preservation, plus low-density-equivalent smoothing on
   high-density meshes without destroying fine detail.
5. **Granular physics:** spherical overlap/cohesion is not full DEM; add
   tangential friction, damping, stable contact cache, active-boundary halos,
   fracture, and collisions with existing sculpted geometry.
6. **GPU capability:** `AvailablePhysics` now has **per-family** capability flags.
   Do not advertise Warp DEM as Warp volume/MPM. Implement a real executor
   registry and measure CPU/GPU transfer break-even before selecting GPU.
7. **Multires safety:** replace sample-order-derived fake tile IDs with actual
   CCG grid/tile identifiers; transfer changes only to the active Sculpt Layer,
   preserve independent layers and per-corner UV/color attributes.
8. **Visual parity:** Native Sculpt's brush alpha/falloff, normals, symmetry and
   automasking must come from Blender's existing APIs, not duplicated in the
   physics solver. Preview sculpting must remain reversible.
9. **Precision:** test zero/near-degenerate tet volume, stiffness scales,
   inverted cages, NaN/overflow, extreme scene coordinates and backtracking.
10. **Keep builds portable:** no unconditional Python, Warp or NVIDIA headers
    in the core build. Blender 5.4 changes should be isolated to the editor
    adapter, not the solver contracts.

## 6. How Codex runs tests

```bash
cmake -S experimental/sculpt_physics_cpu -B build-sculpt-physics -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sculpt-physics --parallel
ctest --test-dir build-sculpt-physics --output-on-failure
python -m pip install numpy
python experimental/sculpt_physics_cpu/gpu/warp_granular_probe.py --device cpu --self-test
```

The real Warp JIT kernels are also tested on the Warp CPU device
(`warp-lang==1.19.0`) in the separate GitHub CI job. Run locally:

```bash
python -m pip install 'warp-lang==1.19.0'
python experimental/sculpt_physics_cpu/gpu/test_warp_xpbd_local.py --device warp-cpu
python experimental/sculpt_physics_cpu/gpu/test_warp_resident_contact.py --device warp-cpu
```

**These tests do not establish CUDA performance or Blender acceleration.**
See the detailed [GPU audit and next gates](gpu/GPU_OPTIMIZATION_CODEX.md).

## 7. What Codex should do first

1. Review and run **`test_sculpt_physics_pipeline.cc`**; fix any CI/MSVC
   failures without weakening existing numerical invariants.
2. Add `NativeBrushAdapter` for an ordinary Sculpt PBVH Mesh:
   exact mask/falloff, unique IDs, one stroke lifetime and native Undo coverage.
3. Bind only **Grab** first; compare byte-level Standard bypass and quantify
   deformation vs old V13, p50/p95 first-dab and continuation.
4. Add Clay and Smooth after Grab passes; add explicit mesh proxy cache lifetime.
5. Never merge PR #24 to `main` while Blender viewport/Multires and device
   parity checks are missing.

The design objective is not simply to collect many solvers. It is **one
predictable Sculpt workflow with multiple internal physics materials**, with
clear fallbacks, stable Blender attributes and measurable speed.
