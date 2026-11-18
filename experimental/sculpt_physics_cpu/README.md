# Sculpt Physics CPU Reference Prototype (experimental, standalone)

This is **working standalone C++17 prototype code**, not a shipped Blender feature or a compiled Blender module. It is deliberately decoupled from Geometry Nodes and the editor to provide a small, testable starting contract for future native integration. Do **not** use as a general collision solver or production-ready volume simulation.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Tests cover Standard/0% Assist bypass, local Grab with distance-bond propagation, plastic creep and fracture, stroke cancellation, floor constraint, and **corner UV seam preservation** by barycentric surface anchors; mask threshold clipping with UV/color/material retention and welded source-edge boundaries.

## Performance and correctness audit (18 November 2026)

See [CPU benchmark, confirmed regressions, fidelity limits and optimization priorities](PERFORMANCE_CORRECTNESS_AUDIT_20261118.md).

Reproduce benchmark (optional target, off by default):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSCULPT_PHYSICS_BUILD_BENCHMARKS=ON
cmake --build build --config Release
./build/bench_physics_core
```

Three corrected issues have regression tests: per-step dirty tracking, clearing stale targets when Assist is 0%, and immediate single-substep bond propagation. The reference still copies the whole world at stroke start, and `World::step` still advances any physics when called; the **Blender brush adapter must bypass the world entirely in Standard/0% mode**.

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
