# Sculpt Physics CPU V2 — Correctness, Fidelity and Performance Review

**18 November 2026 · Experimental branch only · Code is standalone C++17 and has NOT been integrated into Blender.**

## Scope and reproducibility

Reviewed the version of `experimental/sculpt_physics_cpu/` on PR #21, including `physics_core.cc`, `sculpt_surface_bridge.*`, `mask_extract_core.*` and both test executables. Rebuilt from sources using Linux GCC 14.2 (Release, warnings as errors), GCC Debug with AddressSanitizer/UndefinedBehaviorSanitizer, and Clang 17 (Release). **2/2 CTest suites passed in all three configurations after the patches below.** These are standalone tests only; no Windows/MSVC, Blender PBVH, brush invocation, Multires CCG or GPU tests have run.

## Confirmed defects reproduced and fixed

| Severity | Location | Problem | Change | Regression |
| --- | --- | --- | --- | --- |
| **High** | `World::step_local` | A Grab dab selecting no target particles unexpectedly fell back to a full-global simulation, potentially advancing residual velocities across the whole Sculpt mesh on an empty stroke. | With no targets and no global forces/colliders, make the *quasistatic local* path a no-op and clear dirty data/statistics. Global gravity/collision still triggers full simulation. | Test moving a three-particle chain, then missing the mesh with the next dab. |
| **High** | `SculptSurfaceBridge` Multires | Fine detail residual was stored in object coordinates and failed to rotate with the coarse proxy face, resulting in visibly drifting high-frequency detail under large rotations. | Encode residual in the reference triangle's tangent/normal frame, reconstruct it from the deformed frame. Degenerate deformed triangles now explicitly raise instead of returning a silently invalid rotated detail. | Rotate a triangle 90° around X and assert that +Z detail follows -Y. |
| **Medium** | `for_regular_mesh` | Source vertex and proxy particle counts were compared, but not their initial positions. First physics composition could make the sculpt jump to an unrelated proxy. | Check finite coordinates and a scale-aware initial alignment tolerance; reject mismatches. | An intentionally displaced proxy must be rejected. |

**Behavioral contract:** the local no-target early return is for the *quasistatic Sculpt* path. If the user requests ongoing physical motion without a brush target, call the global time-step/scene-simulation interface explicitly. Do not treat the early return as an all-purpose dynamics simulation rule.

## Memory optimizations implemented

- **Ordinary Sculpt:** `SculptSurfaceBridge` now uses direct vertex->particle indices instead of allocating one Sample **plus a copy of the input Vec3** for each base mesh vertex. This avoids approximately **176 bytes per mesh vertex** in bridge-owned storage on the tested 64-bit layout (prior sample ~152B + copied position 24B). Runtime validation of mesh/proxy alignment happens when constructing the bridge. A CPU **1:1 physics proxy for every dense mesh vertex remains unscalable**; decoupled coarser proxies are a separate architectural step.
- **Multires:** a Sample now stores a single `Vec3` with local-frame residual components plus a validity bit, rather than three extra `Vec3` frame axes. The source-position copy is also eliminated. Estimated bridge-owned memory drops from **~176 to ~80 bytes per high-resolution sample** (implementation-specific sizeof/alignment; excluding vector capacity and particle data). At 10M samples this represents an estimated **~0.96 GB saved**, but the remaining 0.8 GB is itself too large for a high-end Multires design.
- **Limitation:** the current frame is still built from the proxy triangle **per sample and per composition**. High-resolution samples sharing a triangle should share cached frame data and be processed in dirty tiles, not iterated globally. No benchmark of the *bridge itself* is yet available; memory estimates are not measured peak RSS.

## Independent solver benchmarks (current local run)

Single-thread GCC 14.2 Release on a Linux virtualized CPU; grid of particles with nearest-neighbor bonds, one warmup + five timed repetitions per mode, median solver-step time only, 4 substeps x 5 iterations. No Blender viewport/mesh updates:

| Particles | Bonds | Global | Local 4 hops | Local 8 hops |
| ---: | ---: | ---: | ---: | ---: |
| 8,000 | 22,800 | 8.105 ms | 0.959 ms | 3.726 ms |
| 27,000 | 78,300 | 31.654 ms | 1.140 ms | 3.685 ms |
| 125,000 | 367,500 | 141.073 ms | 1.248 ms | 3.665 ms |

These are fresh measurements, not proof of a speedup from the correctness fixes; timings depend on host/CPU load. For 125k particles the `begin_stroke` snapshot was **~9.1 ms** (median in an independent benchmark, 5 repetitions) and `apply_grab` full particle scan **~0.34 ms**, neither included in the local step figures. End-to-end brush latency is therefore larger than the local solver time.

## Important unresolved risks — implement before native editor integration

### P0 — Correctness / safety
1. **Local-island accuracy:** the chosen `bond_hops` and fixed boundary are an approximation. Existing tests already show material-dependent errors; 4 hops can visibly diverge from global deformation. Introduce adaptive halo based on boundary motion/residual and compare multi-dab histories, different stiffness and fractures. Do not label fixed-hop local mode physically exact.
2. **No native Sculpt undo / redraw hook:** `cancel_stroke()` restores particles/bonds but also clears the per-step `dirty_` list. A future Blender adapter must explicitly invalidate/repaint all restored dirty PBVH regions, not rely on this post-cancel list alone.
3. **No atomic recovery on solver exceptions:** a nonfinite state detected mid-step throws after other data may have mutated. Add input timestep/bond parameter ranges, failure rollback and/or step-local checkpoints before connecting to live Blender mesh data.
4. **Proxy consistency after nonphysical edits:** ordinary Sculpt can edit its mesh between physics strokes; Multires may also change CCG/attributes/layers. Reinitialize or invalidate stale bridges on geometry generation changes, and keep stable sample correspondence.
5. **Dynamic topology:** broken bonds do not create a tear surface. Do not apply fractured topology to live Multires layers; use a separate simulation object until an explicit topology remapping system is available.

### P1 — Performance / numerical fidelity
1. **Full-stroke snapshots:** `begin_stroke()` copies all particles and bonds even if a brush touches a tiny island. Replace with a sparse rollback journal and test exact restoration of plasticity/fracture state.
2. **Repeated O(N) work:** Grab target scan, `included` array, active BFS, bond-index dedup/sort and allocations repeat every dab. Use a spatial hash and generation-stamped topology/island caches; reuse scratch buffers.
3. **Serial Gauss-Seidel:** investigate reuse of Blender's XPBD constraint coloring to parallelize nonconflicting constraints. Benchmark 1/2/4/8 threads and numerical determinism before GPU backends.
4. **Insufficient material constraints:** distance bonds do not provide real incompressibility, shear/torsion, or validated elastoplastic clay; no particle-particle contacts, stacking or friction. Add volume constraints/continuum proxy, narrowphase and convergence measures.
5. **Surface frame degeneration/shear:** rigid rotations are handled for valid triangles; highly sheared/flipped triangles, collapsed triangles and UV distortion still require robust policy and tests. When the bridge signals invalid transfer, fall back safely rather than blindly writing to the mesh.
6. **High-density Multires memory:** ~80 bytes/sample remains too costly for tens of millions of samples. Cache/tile/stream only dirty sample blocks; use a smaller proxy and geometry-tangent-frame transport.
7. **Mask Extract HR robustness:** the reference clips supplied triangles and preserves per-corner UV/color/material, but has no native SubdivCCG mask sampling, attribute layers, unsupported topology handling, degenerate-face filtering or comprehensive NaN validation. Test threshold-edge cases, UV seams, multiple islands, hole boundaries, non-manifold input and real Multires.
8. **Brush semantics:** `PhysicalAssist` and `FullPhysics` still share the same Grab target generation; the bridge only linearly blends final positions. Physically informed material behavior, pressure/falloff and layer masks remain to be designed.

## Recommended implementation order

1. Finish **robust no-target/invalid-data, rollback, and geometry-generation tests**.
2. Implement cached spatial brush lookup + adaptive active island; benchmark solver **and** journal/bridge/PBVH time, not just physics kernel.
3. Connect to ordinary Sculpt with a true `Standard` no-allocation bypass and safe geometry/Undo transaction (single low-density mesh).
4. Connect the same backend to Multires using dirty CCG tiles and *active layer* deltas, preserving nonactive layers and fine detail.
5. In parallel, develop HR Mask Extract as a separate native path; only later support editable Multires hierarchy and dynamic fracture.

**Status:** three confirmed defects fixed, bridge memory reduced, standalone Release/Sanitizer/Clang test suites pass. No released Blender functionality or production-quality material solver is claimed.
