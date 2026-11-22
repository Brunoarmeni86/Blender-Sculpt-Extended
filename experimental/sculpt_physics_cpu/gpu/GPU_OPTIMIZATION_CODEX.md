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
