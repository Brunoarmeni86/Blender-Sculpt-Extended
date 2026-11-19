# Multi-island XPBD for consecutive Sculpt strokes — V5 experiment

**18 November 2026 | Draft PR #21 | Standalone C++17 research code — NOT in Blender builds.**

## Motivation

V4 correctly tracked residual particle velocities between strokes but paid a substantial global-solver penalty whenever the current Grab island and a previously moving component were disconnected. The existing `step_local()` / `step_local_adaptive()` paths and their explicit safety fallbacks remain unchanged.

**New opt-in API:**

```cpp
world.step_local_multi(1.0 / 30.0, /*substeps=*/2,
                       /*iterations=*/5, /*bond_hops=*/3);
```

It seeds the normal brush-local graph traversal with **both Grab targets and particles with significant residual velocity** from earlier dabs/strokes. The union of those bond-hop neighborhoods is processed in one local XPBD solve. Disconnected components do not require global fallback merely because they are spatially separate.

## Correctness properties

- All selected bonds are sorted by original constraint ID. Individual islands do not race or share write targets; execution remains serial and deterministic in this reference.
- Gravity or ground collision still forces full-world simulation. A missed/empty quasistatic dab stays a no-op.
- The original `step_local()` continues to choose global fallback if motion lies outside its one brush-centered island. The adaptive solver's existing boundary-motion/reaction policy also remains unchanged.
- The multi-island path uses the **same approximate numerical boundary** as fixed-hop local XPBD; it is **not** a guarantee of global equivalence when an island is cut inside a connected deforming material. The test specifically uses complete disconnected material components.
- Registration of old motion is sparse, but every call still allocates the `included[N]` array and takes a full stroke-state snapshot at `begin_stroke()`. No sparse Undo journal yet.
- This is **not** a native Blender brush feature, nor a substitute for volumetric material constraints or particle collisions.

## New tests and local validation

New `test_multi_region.cc`:
1. Two consecutive strokes on two disconnected three-particle bonded components; multi-local is compared to global for all six particles' positions and velocities. The legacy local path is verified to fall back globally.
2. Distinct active components need no fallback when their entire bond neighborhoods fit in the budget.
3. Gravity still requires global fallback.
4. A missed quasistatic dab still has zero side effects.
5. Stroke cancellation restores the previous state.

Local runs (all **six** CTest suites):
- GCC Release — 6/6 passed.
- Clang Release — 6/6 passed.
- GCC ASan + UBSan Debug — 6/6 passed.

GitHub Actions Linux/Windows status must be checked on the exact final commit separately.

## Timing: favorable sparse-interaction scene (NOT representative of dense connected clay)

Virtualized Linux CPU, GCC C++17 Release, single thread, one warmup and six samples per mode, medians. Two three-particle bonded components, both moving, plus thousands of **unbonded stationary particles**. `dt = 1/30`, two substeps × five iterations. Timing covers **solver step only**, excluding the original stroke snapshot, target selection, PBVH, evaluated-mesh transfer and viewport drawing.

| Total particles | Global step | Legacy local (global fallback) | New multi-local step | Particles integrated by multi-local |
| ---: | ---: | ---: | ---: | ---: |
| 1,006 | 0.030 ms | 0.045 ms | 0.002 ms | 6 |
| 27,006 | 1.130 ms | 1.552 ms | 0.005 ms | 6 |
| 125,006 | 6.529 ms | 9.165 ms | 0.044 ms | 6 |

These numbers show the **potential** of sparse multi-region stepping, not a universal speedup. In a dense mesh where most particles are moving or tied together by bonds, the active union can become large and timing can approach the global case. The complete Sculpt brush cost may be dominated by snapshots and surface/PBVH synchronization, none of which were timed here.

## Next engineering priorities

1. **Active connected-component tracking / topology-generation cache** instead of repeatedly allocating an `included[N]` buffer and BFS expansions for every dab.
2. Detect connected material/island boundaries using the **same XPBD reaction diagnostic** as adaptive solving; never silently label a truncated moving region globally accurate.
3. Replace full-world stroke snapshots with an exact **sparse Undo journal** and benchmark the complete stroke lifecycle.
4. Investigate local broadphase and spatial brush selection, and compare serial Gauss-Seidel with existing Blender XPBD constraint coloring.
5. Native Blender Sculpt Mode (regular mesh) Grab adapter with PBVH/Undo; then Multires active-layer writeback, high-resolution detail and UV checks.
6. Multi-material benchmarks: plasticity, fracture, stiffness, repeated alternating strokes and damped motion. CPU-only first, optional GPU later.

**Keep the PR Draft. Do not merge as a production Sculpt physics feature.**
