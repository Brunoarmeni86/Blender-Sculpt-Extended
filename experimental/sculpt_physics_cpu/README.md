# Sculpt Physics CPU Reference Prototype (experimental, standalone)

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
