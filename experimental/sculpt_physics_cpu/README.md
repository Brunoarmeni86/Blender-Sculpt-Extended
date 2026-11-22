# Sculpt Physics CPU Reference Prototype (experimental, standalone)

**Native solver executor contract (new):**
[`PHYSICS_EXECUTOR_CODEX.md`](PHYSICS_EXECUTOR_CODEX.md) describes a typed
`IPhysicsExecutor` API, the standalone CPU XPBD implementation and
full-stroke rollback/commit tests. Native CUDA/Vulkan remain unavailable.

**GPU implementation (experimental):**
[`gpu/GPU_OPTIMIZATION_CODEX.md`](gpu/GPU_OPTIMIZATION_CODEX.md)
documents persistent CUDA local XPBD and granular contact executors,
CPU tests, sparse transfers and benchmarks. CUDA remains unverified on hardware.

**For Codex: start with [CODEX_START_HERE.md](CODEX_START_HERE.md).** It is the current source map, execution contract, recommended order, and verification checklist. This README also retains historical research notes from the standalone CPU prototype.

This is **working standalone C++17 prototype code**, not a shipped Blender feature or a compiled Blender module. It is deliberately decoupled from Geometry Nodes and the editor to provide a small, testable starting contract for future native integration. Do **not** use as a general collision solver or production-ready volume simulation.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Tests cover Standard/0% Assist bypass, local Grab with distance-bond propagation, plastic creep and fracture, stroke cancellation, floor constraint, and **corner UV seam preservation** by barycentric surface anchors; mask threshold clipping with UV/color/material retention and welded source-edge boundaries.

## Already implemented here

- Position/velocity/inverse-mass particles and XPBD distance bonds (in the same mathematical family as Blender's `evaluate_distance_constraint`).
- Iterative constraints, substeps, compliant brush targets and dirty particle report.
- Optional plastic rest-length evolution and breakable bonds (simplified strain criteria).
- CPU-only experimental ground plane contact.
- Stroke begin/commit/cancel, including restoring bond/rest state when canceled.
- Surface-anchor position + per-corner UV interpolation.
- Standalone **Mask Extract reference**: triangles clipped at a mask threshold, edge-boundary vertex welding, original texture seams preserved via UVs/colors stored per face corner, original source untouched.

## Still missing (not disguised as implemented)

- Dynamic broadphase/spatial hashing, particle-particle collisions, friction, full volumetric conservation, robust plasticity and self-contact.
- Multires/PBVH integration, Sculpt Layer writeback, edit safety, Blender Undo and saving `.blend` data.
- Real sculpt brush callbacks, animation/cache and mesher.
- CUDA/Vulkan/Metal compute backends, performance guarantees, high-particle-count scaling.
- **Native Blender** Multires Mask Extract HR is still missing: current reference works on a supplied high-detail triangle mesh, not CCG grids or Sculpt Layers; no rebuild of editable Multires hierarchy.

## Reuse existing Blender code, don't fork it blindly

The Blender fork already has `source/blender/geometry/xpbd/GEO_xpbd_constraint_distance.hh` and the XPBD Geometry Nodes solver. These depend on Blender's `float3`, data spans, geometry refs and node world bundles. Audit a shared adapter/constraint API before moving this prototype into `source/blender/geometry` or wiring it to brushes. The reference code uses small C++17 types solely for independent testing and is expected to be **replaced or adapted**, not maintained as a redundant second solver.

Spec: [`doc/sculpt_extended/NATIVE_PHYSICS_GRANULAR_BRUSHES_MULTRES_20261118.md`](../../doc/sculpt_extended/NATIVE_PHYSICS_GRANULAR_BRUSHES_MULTRES_20261118.md)

Recommended first native integration gate: Grab Physical Assist (opt-in, 0%-OFF identical to existing brush, single layer transaction + local PBVH dirties). Until that gate passes, this code lives in `experimental/sculpt_physics_cpu/` only.

## Normal Sculpt and Multires adapters + local solver (18 November)

This reference now has both a **regular mesh Sculpt** surface bridge (one particle per sculpt vertex) and a **Multires sample bridge** (coarse proxy plus preserved residual fine detail). The bridges are independent of Blender's editor and its Multires modifier; this **does not** yet install a real sculpt brush or write to Sculpt Layers. `Standard` and 0%-Assist always return the passed ordinary brush positions unchanged in the bridge.

`World::step_local` is an optional **approximate quasistatic brush-local XPBD** path using a bond-hop halo and fixed exterior boundaries. Global gravity or floor collision falls back to the global step. Choosing a small halo can cause visible differences from global simulation; **8 hops was much more accurate than 4 hops in the measured test**. For exact full-world dynamics, use `World::step`.

See [local solver/normal sculpt audit](LOCAL_SCULPT_ADAPTER_AUDIT_20261118.md). The [correctness and optimization review](CODE_REVIEW_20261118.md) records three fixed regressions, GCC/Clang sanitizer checks, memory optimizations and remaining blockers before Blender integration. Build with `-DSCULPT_PHYSICS_BUILD_BENCHMARKS=ON` to additionally run `bench_local_physics`; run `ctest --test-dir build --output-on-failure` for both standalone test suites.

## Adversarial audit and optimization research (18 November)

See [additional XPBD optimization research and regression tests](OPTIMIZATION_RESEARCH_AND_ADV_TESTS_20261118.md).
The experimental CPU solver now protects artificially pinned local halo boundaries from
irreversible plastic/fracture edits, rejects infinite bond rest lengths, validates finite
positions and UV/color corners in Mask Extract, and checks normal Sculpt proxy alignment
independently of a large object translation.

The new `test_adversarial.cc` is included in CTest. A locally patched standalone source
copy passed **3/3 CTest suites**, and the adversarial tests passed GCC ASan/UBSan.
This is not a complete CI test of the exact GitHub branch and is not a Blender build.

## Adaptive local XPBD (experimental, 18 November)

`World::step_local_adaptive()` expands the brush-local bond island when the
outer numerical boundary moves too much. It snapshots affected particle/bond
state before each candidate, rolls back rejected trials, and can use **strict
global fallback** or explicitly return a **bounded approximation**
(`converged=false`). Successful halo widths are reused within the same stroke.
The criterion is a **heuristic**, not a proven physical accuracy guarantee.

The standalone project now has a fourth CTest suite (`test_adaptive`) and an
optional `bench_adaptive_physics` target with reproducible comparison of
fixed/strict/bounded/global latency. The strict mode can be **slower than the
global solver** after unsuccessful local attempts. No automatic policy should
be enabled in Blender until editor integration and differential testing.

[Adaptive halo design, benchmarks and limitations](ADAPTIVE_HALO_REPORT_20261118.md).


## Follow-up correctness audit — stiff boundaries and Mask Extract

[False-convergence regression, mask geometry corrections, and CPU optimization priorities](BUG_AUDIT_BOUNDARY_REACTION_MASK_20261118.md).

Adaptive XPBD now checks numerical **boundary reaction** as well as boundary displacement. This fixes a case where a stiff crossing bond canceled Grab entirely yet the solver falsely reported convergence. The Mask Extract reference rejects degenerate output triangles and uses fixed-size clipping buffers rather than per-face dynamic vectors. These remain **standalone experimental changes**, not Blender-integrated features.

## Multistroke motion continuity — CPU V4

The local solver no longer silently freezes moving particles outside a new
brush island. A distant moving component triggers a transactionally guarded
global step; the adaptive solver includes reachable moving particles by
expanding its halo. A sparse mover index avoids scanning every static proxy
particle each dab, and is preserved across stroke commits/cancels.

See [full correctness audit, tests and benchmark limitations](MOTION_CONTINUITY_AND_SPARSE_REGISTRY_20261118.md).
The new fifth CTest suite is `sculpt_motion_continuity_tests`. These are
standalone checks, **not** a real Blender Sculpt/Multires integration.

## Multi-island local XPBD (V5, 18 November)

New opt-in `World::step_local_multi()` combines the current Grab targets and
previously moving particles as separate seeds, expanding through unbroken
material bonds with the same approximate bond-hop halo. This lets two distant,
disconnected active regions advance in one CPU step without paying a full-world
fallback. The original `step_local` and `step_local_adaptive` behavior is
unchanged; gravity and contact still require global integration.

A sixth CTest suite compares two consecutive strokes against the global
reference on separate material components. The optional
`bench_multi_region` target demonstrates sparse-step cost in a favorable
scene with mostly stationary unbonded particles. It does **not** measure
full Blender Sculpt Mode brush latency.

[Multi-island algorithm, benchmarks and limitations](MULTI_REGION_XPBD_AUDIT_20261118.md).

## Hybrid sparse stroke Undo + persistent local buffers (V6)

Local physics dabs now record particle and bond state on **first touch**, rather
than copying the entire proxy at stroke start. If a later dab needs full-world
XPBD, the stroke journal upgrades into a contiguous global snapshot while
preserving original pre-stroke values. Scratch for local bond IDs, active
particles, previous positions, and inclusion generation marks is reused across
dabs. Undo here is a **standalone stroke cancel operation**, not Blender's
native Undo/Redo.

[Design, seven-suite tests, benchmark comparisons and remaining limits](SPARSE_UNDO_PERSISTENT_SCRATCH_V6_20261118.md).
The optional `bench_stroke_undo` CMake benchmark covers stroke startup and local
solver time; full Blender PBVH/Multires/viewport and native Undo costs remain
unmeasured. The global case can still be slower than V5.

## V7 numeric-failure and Grab-target audit (18 November)

The standalone CPU reference now protects each **global, local or multi-island
step** from leaving partially mutated particles, plastic bonds or fracture
state after a numerical exception. This per-step rollback is separate from
the stroke Undo journal. `apply_grab()` stages validated finite targets
before replacement, including a robust huge-radius path. The solver rejects
underflowing squared XPBD timesteps and validates pinned particles.

Two new CTest suites (nine total) cover numeric overflow, previous accepted
dabs, continued valid work after an error, original stroke cancel, invalid
target retention and near-zero timestep stability.

[Reproducers, fixes, test evidence, performance impact and remaining risks](V7_NUMERIC_ATOMICITY_BUG_AUDIT_20261118.md).
**This is not a Blender-integrated feature, and the extra step checkpointing
may slow some brush benchmarks.**

## V8 — Numeric stability and deferred Undo conversion

Two additional regressions were reproduced and fixed: an unsuccessful global
step previously promoted an otherwise sparse stroke Undo journal to a full
snapshot, and extremely small (but positive) XPBD timestep squares could
overflow compliance multipliers, producing NaN bond state without an error.

Successful global steps now reuse their **per-dab rollback snapshot** as the
dense pre-stroke Undo buffer, preserving earlier sparse originals. Failed steps
leave the sparse journal intact. Duplicate full-world fallback snapshots in
the local and adaptive wrappers have also been eliminated.

The tenth CTest suite (`sculpt_v8_transactions_tests`) exercises failed and
successful global transitions, cancel, pinned constraints and finite-alpha
validation. See the [V8 bug audit and benchmark](V8_NUMERIC_UNDO_FALLBACK_AUDIT_20261118.md).
This is **standalone C++ only**, not Blender Sculpt/PBVH/Multires integration.

## V9 — Long-run stiffness tests and cancelled configuration edits

Cancelled strokes now restore gravity, damping and ground settings as well as
the physical particles, bonds and sparse motion state. Committed settings form
the baseline for subsequent strokes; cancelling a later edit restores it.

An eleventh CTest suite exercises **720 alternating reference strokes**:
640 across four distance-constraint stiffness settings and 80 further
plasticity/fracture strokes, comparing complete disconnected components against
the global solver with repeated stroke commits/cancels. These are **CPU
reference tests**, not equivalence guarantees for truncated local islands
or a working Blender Sculpt/Multires brush.

[Minimal bug reproducer, test design and remaining limitations](V9_LONG_STROKE_SETTINGS_AUDIT_20261119.md).

## V10 — Exact Undo membership for rejected dabs

The local solver now removes first-touch journal records introduced by a
**failed numeric step**, and the adaptive halo solver removes records from a
numerically successful but **quality-rejected candidate**. This prevents
unused Undo allocations after repeated failed/retried dabs without disturbing
previously accepted strokes. A reused key scratch and single-pass hash insert
reduce the additional bookkeeping cost.

The Grab radius fast path now falls back to robust `std::hypot` when the
squared radius is zero from underflow (as well as overflow), so even an
extremely small finite brush hits its exact center.

[Reproducers, regression tests, preliminary timings and remaining risks](V10_UNDO_JOURNAL_RADIUS_AUDIT_20261119.md).
The twelfth standalone CTest suite is `sculpt_v10_journal_radius_tests`.
**None of this is compiled into a Blender Sculpt brush yet.**

## V11 foundation: scale-safe XPBD and native-friendly PBVH candidate API

Global, local, and multi-island XPBD no longer drop rigid constraints because
the inverse mass is positive but smaller than a hard-coded `1e-12` threshold.
Bond lengths at extreme (but finite) coordinate magnitudes use a robust
overflow-safe norm path. The new `World::apply_grab_candidates()` accepts
PBVH-like preselected particle indices, sorted and deduplicated internally,
and uses the same target generation path as the existing full-scan Grab.
This keeps Blender's own spatial structure as the preferred source of brush
candidates; a second physics-only spatial hash is not necessary just for Grab.

The 13th CTest suite checks scale robustness and exact indexed/full-scan
parity. `bench_candidate_query` measures selection time **given preselected
indices** and excludes PBVH query cost and all Blender viewport work.

- [V11 architecture / quality and Blender integration contracts](FOUNDATION_ARCHITECTURE_GATE_V11_20261119.md)
- [Step-by-step Windows 11 build and test guide](WINDOWS_BUILD_AND_TEST_20261119.md)

This remains **standalone research code**, not a native Blender sculpt feature.

## V11 addendum — fail-safe Multires sample output

The standalone Multires bridge rejects non-finite rest anchors, detail
residuals and physically reconstructed/blended sample positions before
returning any geometry for editor writeback. This closes a reproduced gap
for degenerate rest frames with invalid deformed proxy coordinates.
`test_multires_safety.cc` brings standalone CTest coverage to **14 suites**.
This does **not** establish native Multires CCG/Undo integration or detailed
shape preservation under arbitrary shear/triangle inversion.
