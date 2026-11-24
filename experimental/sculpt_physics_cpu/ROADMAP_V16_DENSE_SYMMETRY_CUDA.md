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

## 2026-11-24 — compact stroke implementation

Implemented on the experimental V16 branch:
- Fix: Undo-covered halo vertices remain pinned unless explicitly active.
- Fix: CPU executor revisions stay monotonic across stroke and island reuse.
- Fix: avoid native/physical mixing following unsupported geometry fallback, reject clipping changes to stationary vertices.
- Opt-in ordinary-mesh compact CPU path: `BLENDER_SCULPT_XPBD_V16_COMPACT=1`, in addition to the existing V15 build and runtime switches. A 32-bit edge incidence index replaces the full-mesh World in this path. Only the active region and its one-ring halo are instantiated as local physics particles/bonds for each dab. Global/local mapping is sorted, and the original coordinates of written vertices are captured before native writeback. The old CPU path remains the reference.
- No-op dabs avoid rest-world initialization.
- Shared compact adapter and moving-dab CPU parity regression are registered in standalone CTest.

Actual verification: the standalone compact topology test was built with g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG and passed in the assistant's Linux environment. Source blob hashes verified against GitHub: compact topology ad182dc1c16c003410f5cbdbed3e222f7bce50d9; test bd5eb661cc54b0c0b501734fd4da0596e1da8445. On a synthetic 1M-vertex chain (999999 edges), the index reported 11999996 bytes; gathering two adjacent vertices visited four edge references and returned three unique edges/four vertices. This is NOT a Blender viewport or CPU/GPU speed benchmark.

Not executed: full standalone CTest, compact numerical parity test, Windows Blender compilation, runtime, Undo, symmetry, CUDA. Direct repository checkout from the assistant's test environment failed (DNS/network unavailable); GitHub connector commits succeeded.

Remaining limitations: topology indexing is still O(V+E) once per stroke, not persistent across strokes; memory for the sparse rest map grows with the touched region; per-dab World/executor allocation remains local but is not reused. Mesh identity, edge pointer/count and local incidence checks do not prove detection of all same-count in-place topology edits. Masks are sampled at each dab in the compact path, so test static-mask parity first. Fixed one-ring halo only; no unjournaled exterior writes. Keep the compact path opt-in until full parity and native editor gates pass. The symmetry guard remains intentionally enabled and no native CUDA executor has been registered. Do not replace the local V15.1 branch or lose the Multires Separate fix.

## 2026-11-24 — second optimization and parallel/symmetry foundation

- Direct compact island packing eliminates the temporary local World and second halo extraction. Shared prepared-island CPU solve validates topology generation, sorted IDs, masses, bonds, native targets, and exact mobile target coverage. The old full World reference is retained. Numerical parity CTest must be run before promotion.
- Reuse sparse edit and target validation buffers within an executor; actual per-dab executor lifetime remains to be extended.
- Fix GPU relative writeback: apply only the actual relative float delta to the authoritative double position; origin-plus-quantized-position caused untouched-axis drift. Added regression, not run here.
- Added deterministic conflict-free edge constraint batches in constraint_batches.hh. Both endpoints are exclusive in a batch, including pinned endpoints; future parallel in-place kernels can avoid data races if they honor this contract. Colored Gauss-Seidel changes iteration order; numerical comparison against serial and existing Jacobi is mandatory.
- Added candidate mirror-dab collection in symmetry_dab_targets.hh. Duplicate identical same-pass contributions are deduplicated, conflicts rejected, distinct passes reduced in canonical XYZ mask order. Additive overlap is an explicit experiment, not stock-equivalent behavior yet. Actual editor dispatch in sculpt.cc::do_symmetrical_brush_actions calls do_tiled sequentially after computing symmetry feather; collect targets against a common pre-dab snapshot there and stage native Undo coverage before the union solve. Do not reapply feather; keep symmetry disabled until integrated and tested, including failure/rollback.

External research (reference only, no third-party code copied):
- Blender XPBD developer docs: https://developer.blender.org/docs/features/nodes/xpbd_solver/ — native graph coloring approach.
- PositionBasedDynamics XPBD: https://github.com/InteractiveComputerGraphics/PositionBasedDynamics/blob/master/PositionBasedDynamics/XPBD.cpp — constraint equations; https://github.com/InteractiveComputerGraphics/PositionBasedDynamics — parallel solver reference.
- NVIDIA Warp coloring: https://github.com/NVIDIA/warp/blob/main/warp/native/coloring.cpp — comparison for scheduling.
- Newton VBD: https://github.com/newton-physics/newton/blob/main/newton/_src/solvers/vbd/solver_vbd.py — alternative implicit solver, not a drop-in replacement.
- Warp runtime: https://github.com/NVIDIA/warp/blob/main/docs/user_guide/runtime.rst — preallocate buffers, graph capture only when stable and verified.
- Carnage Grains: https://superhivemarket.com/products/carnage-grains — commercial behavior reference, not accessible source code. Granular remains deferred.
- Existing V14/V15 code includes CPU executor and GPU island contract; previous checkpoint names Jacobi and Smooth A3 PTX files. Their source/runtime could not be recovered from the accessible remote commit search. Do not assert that these kernels are usable. Recover from local checkpoint when available.

Actual tests this iteration: independently compiled and ran the new constraint batch and mirror target tests using g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG. Both passed. Git blob hashes of the four local files match remote files: constraint_batches.hh 0a2b0b781b7f6e41e2e8fc186515552cd35f5de7; test_constraint_batches.cc 6a12667e950d5f879283cce86105d60a598a4b94; symmetry_dab_targets.hh e22505d0d19d712cc6b0d538a7e558b21613ebca; test_symmetry_dab_targets.cc 60a9d39df72ce0391f17bfc0663821a6e96ef5de. Synthetic chain of 1M vertices and 999999 edges produces two batches; mirror test checks order, same-pass duplicates/conflicts, overlap, and invalid passes. No GPU or Blender runtime involved. The full repository checkout still fails in this environment (DNS), so no full CTest or Windows Blender build was performed. Direct island and writeback fixes need full regression verification. Next: compile Debug CTest, compare compact and full across moving dabs, profile real PBVH workloads; then cache topology by verified mesh invalidation, persist executor buffers, implement native XYZ dispatch and real CUDA executor. Keep original opt-in flags and local Separate rebase.

## V16-A — first artist trial: Clay / Clay Strips / Draw / Smooth

User priority: ordinary coordinate-only brushes first; defer Grab Tear and all topology-changing material tools. Preserve all UV layers and existing corner mapping, polypaint, mask and Face Sets. No UV relaxation or compensation in this phase. Physical deformation can still stretch the texture geometrically.

Published:
- CPU Colored GS reference (serial color ordering, not threaded or CUDA), opt-in via BLENDER_SCULPT_XPBD_V16_COLORED=1.
- Guard actual stroke mirror axes, reference symmetry and radial counts before the first physical dab. Symmetry remains unsupported for physical V16-A.
- Optional bounded material overrides: BLENDER_SCULPT_XPBD_V16_BOND_SCALE (0..1000; smaller is stiffer), BLENDER_SCULPT_XPBD_V16_REST_FOLLOW (0..1; larger follows the native target edge lengths), BLENDER_SCULPT_XPBD_V16_TARGET_COMPLIANCE (0..1; smaller holds target more strongly). Missing/invalid overrides retain the selected profile. These are process-level trial knobs, not persistent Brush Asset controls or calibrated materials.
- verify_blender_brush_trial.py compares real saved mesh snapshots for topology, all UV layers, color attributes, mask/Face Sets/hidden attributes, geometry movement, optional Undo equality. This script is not yet run in Blender. It verifies attribute invariance, not physical-backend activation.

Compile the existing V15 research options (WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL, WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14, WITH_SCULPT_XPBD_OTHER_BRUSHES_V15). Use a separate worktree/build/install or selectively integrate into local V15.1 after checking the Separate rebase. Run standalone CTest Debug before attempting native sculpt trials.

First launch: BLENDER_SCULPT_XPBD_OTHER_BRUSHES_V15=1, BLENDER_SCULPT_XPBD_V16_COMPACT=1, BLENDER_SCULPT_XPBD_V15_PROFILES=1, BLENDER_SCULPT_XPBD_V15_ASSIST=0.5. Leave V16_COLORED=0 for baseline, then compare with 1. Disable object XYZ symmetry, radial/tile/reference symmetry, Multires, Dyntopo, shape keys and deform modifiers for initial trial. Test Clay, Clay Strips, Draw, Smooth individually on a UV-mapped ordinary mesh; check console for V16 physical brush CPU XPBD compact preview ACTIVE (or existing reference log), and verify saved snapshots with script. Compare fully masked vertices and seam UVs. Start with 100k–500k vertices, then 1M and 5M+ only if stable. Repeat with native path and assist=0.25/0.5/1.0. A saved Undo snapshot must be made after actually undoing the stroke.

No full compile, CTest run, native UV/Undo test, GPU or symmetry integration verified in this iteration. The prior remote checkout limitation remains. Keep all experimental features off by default and do not release this branch as a verified build.

## V16-B — Physical Clay V2 and native artist controls

User V15.1 trial: CPU XPBD active after disabling legacy Sculpt symmetry X flag as well as mesh symmetry. Visual behavior close to stock Clay; first stroke slower at high mesh density (qualitative, no solver timing measurement).

Published:
- Physical Material for Clay and Clay Strips: per-dab edge yield approximation and bounded lateral pressure redistribution between writable vertices. The native goal is retained for assist blending. No true persistent plastic strain, collision, volume conservation, topology edit, CUDA or AVBD is claimed.
- Per-Brush DNA/RNA mode (Launch Settings, Native, XPBD Assist, Physical Material), assist, yield, plasticity and pressure spread. Settings are snapshotted per stroke; editable in Advanced. Older assets initialized using missing DNA field check. Physical mode selects compact path, and Clay Material profiles; other brush kinds remain assist-only.
- MeshRuntime shared cache of immutable compact edge connectivity. Reuse across strokes, invalidate on clear/topology changes and edge splitting. No coordinates/UV data in this cache. Cold construction remains O(V+E); repeated strokes should avoid it when the base runtime is retained. Console BUILT/REUSED verifies this empirically.
- Explicit fallback diagnostics; new test cases for tangential movement, blending, pinned boundary and invalid material values.

Initial test: ordinary UV-mapped mesh, XYZ/radial/tile/reference symmetry off, no deform modifiers/shape keys/Multires/Dyntopo. In Advanced choose Physical Material, assist 1.0, yield .035, plasticity .65, pressure spread .55 (try 0 and 1 for comparison). On Clay or Clay Strips check console for "CPU XPBD compact Clay V2 preview ACTIVE". Compare against Native and XPBD Assist at identical stroke settings. Repeat with 100k, 1M and 5M vertices; compare first and second stroke with BUILT/REUSED. Use verify_blender_brush_trial.py on saved before/after/Undo snapshots; actual UV, mask, attributes and Undo verification required.

Standalone CTest (Debug assertions on): cmake -S experimental/sculpt_physics_cpu -B <separate-build-dir>; cmake --build <separate-build-dir> --config Debug; ctest --test-dir <separate-build-dir> -C Debug --output-on-failure. Then full Windows Blender build with the three V15 research switches ON. Preserve local V15.1 Separate/Multires fixes; do not reset the existing local worktree. No build, CTest, real Blender V16-B or UV/Undo runtime result has been verified in this iteration. UI properties are present even in builds without the research compile flags; branch intended for experimental builds only.

## V16-C — Clay V3 footprint response (artist trial)

V16-B user runtime log confirmed compact Clay V2 active and repeated topology cache REUSED; user found visual differences insufficient. User also reported 27/27 standalone tests passing on V16-B and successful Windows build/runtime. V16-C is a new unverified revision.

- Clay Behavior selector inside Physical Material: V2 Reference, Deposit, Compress, Flow. Existing brush assets default to Deposit; launch-environment-only path retains V2.
- Deposit redistributes signed normal increment from center to shoulder across the brush radius. Compress changes center/rim displacement, balancing an edge-length-squared area-weighted projected target budget at spread=1 before XPBD. Flow uses projected object-space stroke motion, not mesh edge length. All use already evaluated native targets; zero/masked/hidden/Undo-excluded samples remain pinned. V3 still uses local XPBD; no true material volume constraint, persistent plastic state, AVBD or CUDA.
- New two-density synthetic tests compare center and rim displacement, approximate balance, Flow and assist=0. Tests added, not run yet for V16-C.
- First nonempty dab diagnostics: targets, staged writes, local prepare_ms (excludes full topology build, editor write and viewport), max native/physical displacement, max deviation divided by max native displacement. No timing or quality claims before artist validation.
- Start with Clay or Clay Strips, Physical Material, Compress, Assist=1 and Pressure Spread=1, no symmetry/Multires/Dyntopo. Compare V2 Reference on identical geometry. Flow needs a moving stroke. Repeat with coarse/dense UV-mapped meshes. Run verify_blender_brush_trial.py with before/after/Undo snapshots. UV coordinates are not edited; texture may stretch as geometry moves.
- Next: actual area/volume constraint, persistent material history with Undo transaction, parallel/CUDA, symmetry. Do not claim these implemented.
