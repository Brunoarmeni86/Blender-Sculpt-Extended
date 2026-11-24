# V16 — dense sculpt physics, symmetry, CUDA

Status: experimental development branch; no release or verified performance claim.
Remote base: 7927fc179ea5c746ddc45394681c0051943b7676 (V15 research branch).
The user's local V15.1 includes the V14 Multires Separate rebase and possibly other local fixes not available on this remote base. Integrate the new commits selectively into that local branch; do not reset it to this branch.

## P0 — Dense mesh CPU

Initial changes: reuse World bond incidence rather than allocating a duplicate CSR index; reserve particle, bond and adjacency storage; keep stable insertion order, topology count and generation checks. Add workset regression coverage. The Smooth span compilation fix is carried forward. These edits have not yet been compiled or benchmarked.

Remaining major bottleneck: the V15 bridge still creates a full rest World once per stroke. Implement a compact topology cache with a reliable mesh identity/version and invalidation on topology changes, Undo, object replacement and mode exit. Separate connectivity from per-stroke rest positions and pinned states. Build local PBVH-derived active IDs and a pinned one-ring halo; extend to adaptive halo only after recording the expanded Undo scope. Preserve initial rest length semantics for vertices visited later in the stroke. Keep exact CPU path as reference; no silent approximate behavior. Reuse executor and scratch capacity where correctness permits.

## P0 — X/Y/Z symmetry

Collect all native mirror-pass translations against a common pre-dab snapshot, preserving stock mask/pressure/texture factors. Define deterministic overlap reduction matching native brush intent, including points on mirror planes. Solve the union of regions once per dab; shared vertices must be written at most once. Compare pass-order permutations, overlapping and disjoint islands, asymmetric meshes and pinned boundaries. Do not simply remove the current symmetry guard. Radial/tile/topological/posed symmetry are separate later gates.

## P0 — CUDA

Audit existing Warp/Jacobi and Smooth A3 kernels and IPhysicsExecutor interfaces. Reuse the current GpuLocalIsland transfer contract and relative float32 packing. Validate CUDA against CPU reference on identical target sets, pinned halos, material profiles and extreme coordinates; report float tolerances. Persist device allocations over dabs; upload dirty ranges only. Select CPU/GPU using measured crossover and actual availability, independently of Cycles CUDA. No CUDA dependency for CPU-only builds. No mid-stroke backend switching after geometry writes without transactional rollback. Keep GPU disabled until numerical and Blender runtime gates pass.

## P1 — Unified Physics UI

Native per-Brush DNA/RNA settings, default off; configuration snapshot per stroke. Expose only wired parameters and actual backend capability. Surface fallback reasons. Preserve legacy environment switches for explicit tests. Validate brush asset save/load, old assets, Undo and native-off equivalence.

## Deferred

Multires/CCG and Sculpt Layers physics after ordinary dense meshes, symmetry and CUDA are stable; retain existing separation and displacement fixes. Granular and additional materials after foundational gates.

## Acceptance and measurement

Run standalone CMake/CTest in Debug (assertions enabled), especially workset, executor and native brush adapter tests; then Windows Blender build with the same V15 compile flags, runtime tests on 100k, 500k, 1M, 5M, 10M, 20M vertices. Vary brush footprint independently of mesh density. Measure first-dab latency, p50/p95/p99, topology setup, CPU solver, transfers, writeback, RAM/VRAM and symmetry error. Compare stock, old CPU, optimized CPU and CUDA. Target 16–33 ms per typical dab is aspirational, not measured. Preserve masks, native clipping, UV, topology, native Undo and fallback behavior. Confirm local V14 separation and Texture Displacement UV regressions separately. Document environment, commit and actual test results before promoting.
