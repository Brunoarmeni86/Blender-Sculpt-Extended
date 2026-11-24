# GPU optimization prototype — Sculpt Physics
**2026-11-22 · PR #24 · Experimental · Start with `CODEX_START_HERE.md`**

## What was actually implemented

GPU work is now more than the original `warp_granular_probe.py`.
There are **two independent, local, device-resident experimental
implementations** under `experimental/sculpt_physics_cpu/gpu/`:

| File | Purpose | What persists between dabs |
| --- | --- | --- |
| `warp_resident_contact.py` | Local sphere contacts and cohesion in Warp CUDA | Particle coordinates, inverse masses, radii, ping-pong buffers, HashGrid |
| `warp_xpbd_local.py` | XPBD distance constraints for local Sculpt proxies, using edge graph coloring | Proxy positions, bonds/rest lengths, compliance and XPBD buffers |
| `test_warp_resident_contact.py` | CPU oracle, local edits, sparse readback, invalid inputs | CPU regression always; CUDA optional |
| `test_warp_xpbd_local.py` | Colored constraint independence, pinned particles, repeated dabs | CPU regression always; CPU-vs-CUDA optional |
| `bench_warp_backend.py` | p50/p95 total standalone dab, initial upload and transfer counts | Run on NVIDIA workstation |
| `warp_granular_probe.py` | Minimal original reference for one-off contact projection | Nothing: retained as a baseline |

## Opt-in XPBD CUDA Graph replay (22 November 2026)

The independent `LocalXpbdSession` prototype now supports
`graph_capture=True` on real Warp backends (`device="warp-cpu"` for CI
or `device="cuda"` on an NVIDIA workstation). This does **not** enable
the feature in Blender's native C++ executor.

- The **first** solver call runs normally to compile kernels outside capture.
  From the second call, a Warp `ScopedCapture` records the fixed sequence:
  clear bond/target lambda buffers, project compliant or hard artist targets,
  iterate edge-colored XPBD bonds, restore hard targets where applicable.
- The cache key contains `dt²`, solver iteration count and the **number**
  of active targets. New target IDs and positions with the same count update
  resident input buffers, so graph replay does not freeze brush goals.
- At most four graph variants are retained per island; new target buffer
  allocation invalidates the graph cache and, on CUDA, first synchronizes
  queued work to prevent use-after-free. Different topology means **create
  a new session**; do not reuse captured pointer data across proxy rebuilds.
- `graph_capture=False` remains the default. NumPy CPU oracle explicitly
  rejects capture. `graph_stats` exposes capture/replay/cache counts.
  CUDA hardware/performance is not yet verified on an NVIDIA workstation.
- `test_warp_graph_capture.py` compares **actual captured Warp kernels**
  against uncaptured launches on an identical mutable proxy in hard and
  compliant modes, with changing target IDs, counts, dt, and iterations.
  The Warp CPU variant runs in CI; CUDA remains an explicit manual gate.

Usage:

```bash
python experimental/sculpt_physics_cpu/gpu/test_warp_graph_capture.py --device warp-cpu
python experimental/sculpt_physics_cpu/gpu/test_warp_graph_capture.py --device cuda
python experimental/sculpt_physics_cpu/gpu/bench_warp_backend.py --solver xpbd --device cuda --particles 20000 --dabs 60
python experimental/sculpt_physics_cpu/gpu/bench_warp_backend.py --solver xpbd --device cuda --particles 20000 --dabs 60 --graph
```

Compare two benchmark runs on the **same warmed device**, record p50/p95,
total init cost, sustained performance and VRAM. A graph is NOT guaranteed to
be faster: short batches, varying topology, target sizes and expensive
readback may outweigh saved launch overhead. Benchmarks remain standalone
proxy measurements and omit native PBVH, CCG, Sculpt Layers and Undo.
Reference: [Warp 1.19 graph capture docs](https://nvidia.github.io/warp/v1.19/user_guide/runtime.html#graphs).

## C++ native workset bridge (new)

- `../gpu_workset.hh/.cc` builds a compact local physics island from
  active PBVH proxy IDs, a surrounding halo and native Undo-covered IDs.
  Non-covered halo vertices are pinned, and crossings to omitted bonds are
  rejected unless approximate truncation is explicitly requested.
- `../test_gpu_workset.cc` validates global↔local ID mapping, pins,
  missing boundary bonds, finite output and staged sparse GPU writeback.
- **This does not launch CUDA.** It is the missing typed data-transfer
  contract for a future native C++ `IPhysicsExecutor`. Codex should
  map `GpuLocalIsland` positions, masses and bonds to `LocalXpbdSession`
  buffers, then replace the Python runtime with native Warp AOT/CUDA
  integration after device validation.

## Focused performance and correctness audit (22 November, follow-up)

New native C++ and Warp improvements:

| Change | Implementation | Why it matters |
| --- | --- | --- |
| Indexed local worksets | `GpuTopologyBondIndex` in `../gpu_workset.hh/.cc` | CSR bond incidence is built once per topology generation; a local dab visits only adjacent bonds, rather than scanning ALL mesh bonds |
| Workset parity and invalidation | `../test_gpu_workset.cc` | Indexed vs full-scan equality, incomplete halo behavior, incorrect generations and cross-world use are checked |
| Precision far from origin | `pack_gpu_island_relative`, `stage_gpu_relative_writes` | Converts a compact proxy to float32 relative to an island-local double origin; rejects changes to fixed halo and avoids writing back float32 rounding noise |
| Explicit plastic/rest state | `LocalXpbdSession(..., rest_lengths=...)` | Uses native C++ XPBD bond rest lengths instead of silently rebuilding them from already deformed positions |
| Hard-target drift fix | `warp_xpbd_local.py` | Reapplies kinematic target constraints after each bond-color iteration, preserving requested target position |
| CPU workset benchmark | `../bench_gpu_workset.cc` | Separate index construction latency from complete local island packing; measure p50/p95 on the same host |

The CPU workset benchmark is included in the optional CMake benchmark target.
Its output is NOT evidence of a CUDA GPU speedup. The local CSR index
must be invalidated after topology changes. Fracture state can be filtered
without rebuilding the incidence list if bond indices remain stable.

**Important integration requirement:** the production path should convert
`GpuLocalFloatPack::relative_positions` into Warp `wp.vec3` float32
buffers, keep `GpuLocalFloatPack::origin` stable for the lifetime of that
device island, and reconstruct world-space doubles only for dirty output
vertices. Current Warp scripts operate on already-local positions. The
C++ contract is tested, but a native CPU-to-CUDA upload bridge remains TODO.

```bash
cmake -S experimental/sculpt_physics_cpu -B build-gpu-perf \
  -DCMAKE_BUILD_TYPE=Release -DSCULPT_PHYSICS_BUILD_BENCHMARKS=ON
cmake --build build-gpu-perf --target bench_gpu_workset --parallel
./build-gpu-perf/bench_gpu_workset 125000 100
```

### Measured CPU workset preparation (GitHub Actions, Linux Release)

Source: [CI run 45024496154](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/actions/runs/45024496154).
One deterministic linear-chain topology; small (17-vertex) worksets;
results are a **single CI-host measurement** and must be repeated on the
artist workstation, more-connected organic meshes, and multiple brush radii.

| Full proxy | Index construction (once) | Previous full bond scan p50 / p95 | Cached CSR p50 / p95 |
| ---: | ---: | ---: | ---: |
| 12,000 verts | 0.241 ms | 0.1518 / 0.1728 ms | 0.0013 / 0.0019 ms |
| 125,000 verts | 3.857 ms | 1.5919 / 1.6288 ms | 0.0029 / 0.0033 ms |

The 125k case saves ~1.589 ms **only in local workset preparation** per
dab after initial index construction; in this particular synthetic case
the index construction cost is recovered after roughly three local dabs.
Do not extrapolate this ratio to overall Sculpt latency or CUDA speed.
Benchmarks exclude GPU transfer and kernels, Native PBVH, Multires CCG,
Sculpt Layers, Undo, viewport/draw and dynamic topology updates. CSR
requires additional memory (roughly two bond-ID entries per intact bond
plus N+1 offsets, ignoring allocator overhead).

## Warp compliant sculpt targets (new)

`warp_xpbd_local.py` now accepts
`LocalXpbdSession(..., target_mode="compliant")` and
`set_targets(ids, goals, target_compliance=...)`. Compliance accepts
a scalar or a per-target array. The previously tested hard-target mode
remains the default, so existing experiments are not silently changed.

The compliant kernel projects each target using the same XPBD form as
the native C++ `World` / `CpuXpbdExecutor`. Per-target vector Lagrange
multipliers are reset each separate dab and accumulated across solver
iterations. The artist target is solved before each colored bond pass,
rather than forcibly re-pinned after the pass. Hence a **soft** target
can move with material constraints; it is not guaranteed to finish
exactly at the artist's goal. It is distinct from the previous rigid
kinematic mode.

The `test_warp_xpbd_compliant.py` suite tests varying compliant
targets, fixed boundary vertices, invalid target inputs, multi-dab
parity between NumPy and the real Warp CPU JIT kernel, and a
closed-form one-particle XPBD result.

**Critical readback bug fixed:** Warp CPU `.numpy()` on a reused
readback buffer can expose aliasing; a snapshot retained by a caller
could be overwritten on the next dab. `read_positions` now returns
its own NumPy copy, verified by repeated-read tests. The original
parity discrepancy was traced to this ownership bug, **not** a proven
error in the XPBD constraint equations. This ownership rule must also
apply to the future native C++ CUDA readback buffer and editor staging.

This is still **not** CUDA hardware validation or complete C++/Warp
parity for long deforming meshes. Follow up with differential tests on
identically ordered, pinned and compliant constraint graphs.

## Cross-language XPBD parity gate (new)

The CI now compiles `../emit_xpbd_reference.cc` against the **actual
C++ `World`**, with a pinned boundary, two distance bonds and a
compliant artist target. It then runs the same one-substep graph through
`LocalXpbdSession(target_mode="compliant", device="warp-cpu")` and compares
all three resulting particle positions.

[GitHub Actions cross-language success](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/actions/runs/45029548847)
includes the real Warp 1.19.0 CPU JIT compile, the native C++ World fixture,
and the numerical parity test. This validates one defined physical feature
subset, **not** complete CPU-vs-CUDA physics or native Blender behavior.

```bash
cmake -S experimental/sculpt_physics_cpu -B build-interop \
  -DCMAKE_BUILD_TYPE=Release -DSCULPT_PHYSICS_BUILD_INTEROP_TESTS=ON
cmake --build build-interop --target emit_xpbd_reference
python -m pip install numpy 'warp-lang==1.19.0'
python experimental/sculpt_physics_cpu/gpu/test_warp_xpbd_compliant.py \
  --device warp-cpu --cpp-reference ./build-interop/emit_xpbd_reference
```

Add the next tests with explicit physical feature parity: a heterogeneous
4–8 vertex organic patch, varying inverse mass, overlapping influences,
2–4 substeps, motion continuity and long strokes. Document feature gaps
before broadening tolerances.

## Why these optimizations matter

### 1 — Persistent GPU arrays
A Sculpt stroke contains many consecutive dabs. Allocating a new Warp World
and copying all N vertices per dab is expensive. `ResidentContactSession`
and `LocalXpbdSession` upload their **compact brush island once** and
recycle device arrays across subsequent calls.

### 2 — Local physics worksets
Build a reduced graph from PBVH-selected vertices **plus** necessary physical
halo and fixed exterior. Do NOT upload 20-million-vertex Multires geometry
to the solver. Caller must supply complete relevant contact neighbors and
global→local proxy IDs. This mapping is still **TODO** in the Blender adapter.

### 3 — No per-dab full readback
The CUDA methods `step()` and `solve()` leave geometry on-device. Only
explicit `read_positions(ids)` transfers selected IDs to the CPU. If
necessary, stage those in one Undo-covered PBVH/CCG transaction.

### 4 — Graph coloring for GPU XPBD
`warp_xpbd_local.py` precomputes disjoint bond sets: same-color bonds do not
share vertices, so independent constraints execute in parallel **without
racing on particle positions**. Colors run sequentially, so this is one
Gauss–Seidel-like prototype. Compare it with Jacobi and Blender's existing
XPBD coloring before deciding the production solver.

### 5 — Avoid unnecessary GPU use
Tiny local dabs often favor optimized native C++ because GPU kernel-launch,
serialization and synchronization overhead dominate. Treat
`gpu_break_even_particles` as a **benchmark result**, not an arbitrary
guarantee. The CPU C++ `World` remains the fallback.

## Correctness / important limitations

- These are Python/Warp research executors, **NOT** connected to
  `CpuStrokePipeline`, native PBVH, Undo, Multires CCG, or Sculpt Layers.
- The graph-colored XPBD prototype uses **hard position targets**, not
  the existing `World`'s compliant XPBD target constraints; expect
  numerical differences. No volume, bending, plasticity or fracture pass yet.
- DEM remains simplified contact/separation/cohesion, without robust friction
  history, colliders or particle-to-surface reconstruction.
- The numerical regression CI currently uses **CPU NumPy oracles**, not
  NVIDIA hardware. Kernel compilation, device memory behavior and timing
  on CUDA need verification on a real GPU.
- GPU output must be finite and **fully enclosed by native Undo coverage**.
  GPU update submission must not write directly into Blender's viewport mesh.
- The CPU oracle for DEM is O(N²) and exists for tiny test fixtures; do
  **not** compare its runtime to Warp as an artist-facing speedup over
  the optimized CPU C++ hash-grid solver.

## Run

```bash
python -m pip install numpy
python experimental/sculpt_physics_cpu/gpu/test_warp_resident_contact.py --device cpu
python experimental/sculpt_physics_cpu/gpu/test_warp_xpbd_local.py --device cpu

# On Windows with NVIDIA CUDA and Warp installed:
python -m pip install warp-lang
python experimental/sculpt_physics_cpu/gpu/test_warp_resident_contact.py --device cuda
python experimental/sculpt_physics_cpu/gpu/test_warp_xpbd_local.py --device cuda

# Microbenchmarks; includes per-dab sparse readback, but NOT Blender:
python experimental/sculpt_physics_cpu/gpu/bench_warp_backend.py --solver xpbd --device cuda --particles 20000 --dabs 50
python experimental/sculpt_physics_cpu/gpu/bench_warp_backend.py --solver granular --device cuda --particles 10000 --dabs 50
```

### Numerical acceptance for Codex

1. **First** make CPU reference tests green on Linux and Windows/MSVC
   and CUDA smoke/parity tests on target NVIDIA GPU. Check zero-distance,
   pinned vertices, masks, duplicate IDs, graph coloring independence,
   NaN/overflow, and empty worksets.
2. Test many dabs with moving target sets and long strokes; read every
   edited vertex, not only the brush center. Compare with the existing
   C++ `World` under equivalent rest lengths, stiffness and constraints.
   Note where hard goal vs compliant goal changes the physics.
3. Introduce a native **`IPhysicsExecutor`** interface:
   `allocate_island`, `upload_dirty`, `set_targets`,
   `step`, `download_dirty`, `commit`, `cancel`,
   `capabilities`. Keep native Undo ownership entirely in Blender;
   no Python calls per dab in production.
4. Use Warp **ahead-of-time CUDA kernels or a C++-loadable native library**,
   persistent CUDA streams and optional graph capture **only after**
   numerical parity. Ensure no mandatory Warp/Python/CUDA dependency for the
   regular C++ Blender build.
5. Benchmark local island sizes 1k, 10k, 100k, 1M, active ratios 0.1%–50%,
   varied graph color counts and number of dabs; record full transfer,
   kernel, editor writeback and redraw p50/p95. Choose CPU if faster.
6. Multires GPU path must accept a stable coarse cage, compute only dirty
   CCG sample deltas and maintain per-corner UV/polypaint, Sculpt Layers,
   surface frame continuity and native Undo. Current `MultiresDirtyMap` is
   sample-indexed, NOT an actual Blender CCG grid map.
7. Long-lived GPU buffers need explicit device/driver failure handling,
   context destruction, topology generation invalidation and cancellation
   semantics; if CUDA becomes unavailable, fall back to CPU safely.

## Upstream

Warp HashGrid and graph capture usage were checked against NVIDIA's
`warp/examples/core/example_dem.py` and
`warp/examples/core/example_graph_capture.py` API examples.
Original source in this branch is GPL-2.0-or-later or Python reference;
no third-party Warp example source was pasted into the repository.

**Do not merge to main until actual Blender viewport + Undo + GPU tests pass.**


## Kernel execution audit — beyond the NumPy oracles (2026-11-22)

This branch now includes an independent GitHub Actions
`warp-kernel-cpu` job that installs Warp and compiles/runs the actual
`@wp.kernel` functions on Warp's **CPU device**, in addition to
the NumPy oracle and standalone C++/MSVC checks.

- [First real Warp CPU kernel CI success](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/actions/runs/45025703347):
  Warp 1.19.0 compiled and executed the graph-colored XPBD kernels and
  the HashGrid DEM contact kernels, with comparison to the NumPy oracle.
  The runner had **no NVIDIA CUDA driver**.
- `warp_resident_contact.py` now rejects artist edits to pinned
  (zero-inverse-mass) granular halo particles.
- `warp_resident_contact.py` reuses GPU/Warp sparse IDs, values and
  readback arrays. Growth is geometric, only when the selected workset
  exceeds the current capacity. `wp.copy(..., count=K)` transfers K
  positions/IDs, not the whole capacity. Only a short-lived **host** source
  array is allocated per call; no promise of pinned/async transfers yet.
- `warp_xpbd_local.py` now also reuses sparse buffers, BUT keeps
  target-ID/goal arrays **separate from readback ID/result arrays**, to
  prevent a read between `set_targets()` and `solve()` from overwriting
  the scheduled brush target.
- [Warp CPU scratch regression](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/actions/runs/45025939759):
  repeated sparse contact edits/readbacks consumed **8 tracked Warp
  device-array allocations**, versus 35 in the old test scenario.
  XPBD separately asserts zero allocation growth at fixed workset size.
  These are the prototype's allocation counters, not measured CUDA VRAM.
- The latest real-Warp test uses `warp-lang==1.19.0`; pin this version
  in CI to make API/kernel regressions reproducible.

### Remaining opportunities, in implementation order

| Priority | Improvement | Where | Acceptance gate |
| --- | --- | --- | --- |
| **P0** | Validate device state and rollback after an interrupted stroke; release/rebuild buffers when topology generation changes | Native `IPhysicsExecutor` and `GpuTopologyBondIndex` | Inject failure between solve and staged write; no partial Blender/World state |
| **P0** | Track and preserve exact normal Sculpt brush target semantics (PhysicalAssist once-only blend, full per-vertex factor) rather than current GPU **hard-target** proxy | `warp_xpbd_local.py` + C++ `World::BrushTargetInput` | CPU-vs-Warp full target/bond residuals over 100 strokes |
| **P1** | Pre-allocate pinned host upload/download buffers, reuse CUDA streams, avoid implicit `.numpy()` synchronizations inside each dab | Native CUDA bridge | Device timeline proves reduced syncs and no repeated VRAM allocs |
| **P1** | Batch equal-color constraints and benchmark fewer kernel launches; consider Jacobi/fused loops or CUDA Graphs when graph topology remains fixed | XPBD executor | GPU kernel count, total p50/p95 and numerical error compared on 1k–1M island sizes |
| **P1** | Keep active halo physically complete and detect large boundary reactions; grow workset only when needed | Native PBVH adapter + CSR worksets | No visible seams/popping at region border; strict mode always safe |
| **P1** | Replace one-size GPU threshold with measured cost model using active vertices, bond colors, transfers and first-dab warmup | `solver_policy.cc` | CPU wins for small dabs; GPU selected only if faster end-to-end |
| **P2** | Surface/Multires dirty **true CCG grid IDs**, active Sculpt Layer deltas, corner UV/color/Face Sets and native Undo | Native Blender CCG writer | 3+ levels, multiple layers, save/reload, seams, symmetry tests |
| **P2** | More accurate DEM friction history, contact persistence and particle boundary colliders; MPM plasticity for clay/mud | Granular + future MPM | Material consistency and CPU/GPU parity under long strokes |
| **P2** | Coarse proxy construction and feature-preserving detail residual with dynamic error-driven resolution | `sculpt_surface_bridge` | Detail/volume error and viewport latency measured against 2M–20M mesh |
| **P3** | Portable Vulkan/AMD/Intel backend after CPU/CUDA numerical parity | Separate compute backend | Same material tests, no mandatory NVIDIA dependency |

**Release criterion:** Warp CPU compilation does NOT validate CUDA kernel
execution, GPU memory hazards, PCIe overhead, Blender Undo or actual
Multires. Those are separate acceptance gates; keep stable builds CPU-first.
