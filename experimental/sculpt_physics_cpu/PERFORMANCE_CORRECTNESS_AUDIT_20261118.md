# CPU XPBD Prototype — Performance and Correctness Audit
**18 November 2026 | Experimental branch only | Not Blender-integrated**

## Reproducible measurements

Build (GCC 14.2, Linux virtualized AMD EPYC 9V74, five virtual CPUs visible; benchmark itself single-thread):

```sh
cmake -S experimental/sculpt_physics_cpu -B /tmp/sculpt-physics-bench \
  -DCMAKE_BUILD_TYPE=Release -DSCULPT_PHYSICS_BUILD_BENCHMARKS=ON
cmake --build /tmp/sculpt-physics-bench --config Release
ctest --test-dir /tmp/sculpt-physics-bench --output-on-failure
/tmp/sculpt-physics-bench/bench_physics_core
```

Inputs: regular cubic particle lattice, each particle bonded to its positive X/Y/Z neighbors, compliance `1e-5`, spacing `0.075`, one Grab on a sphere of radius `0.2`, `dt=1/30`, **4 substeps x 5 constraint iterations**, no gravity/collision, one warmup + 5 measured runs. Median wall-clock timings in milliseconds for **the updated code**:

| Particles | Bonds | Stroke snapshot | Grab region scan | Solver step | Dirty particles |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 2,700 | 0.007 | 0.002 | 0.796 | 1,000 |
| 8,000 | 22,800 | 0.079 | 0.012 | 8.419 | 7,099 |
| 27,000 | 78,300 | 0.442 | 0.057 | 31.781 | 14,893 |
| 125,000 | 367,500 | 7.962 | 0.313 | 155.459 | 23,710 |

These timings are *illustrative* and may vary on other machines. The build contains **no PBVH, viewport upload, Multires layer transfer, collision broadphase, self-collision, Geometry Nodes, GPU code or full Blender integration**. Single-shot un-warmed results before changes were slower/variable and are **not** suitable evidence of speedups from a particular patch.

Memory: `sizeof(Particle)=64` bytes and `sizeof(Bond)=72` bytes on the test compiler. At 125k particles + 367.5k bonds, current arrays plus full stroke snapshots alone are ~68.9 MB; the two temporary `Vec3` position arrays used in step add ~6 MB (at peak). This excludes capacities, targets, dirty IDs, surfaces, acceleration structures, Undo and Blender data.

## Confirmed correctness bugs (fixed in this experimental branch)

1. **Wrong dirty update window:** reporting changes against the *stroke start* misses a particle that moves back to its initial coordinates on a subsequent dab. Fixed by comparing every step against its own starting positions; regression test added. This currently costs one extra O(N) position buffer and scan; future update should record dirty solver blocks directly.
2. **Stale Grab constraint when assistance turns OFF:** `apply_grab(... influence=0)` returned without clearing the previous target. Fixed by clearing targets on OFF/0%. Note that **a caller must also bypass `begin_stroke` and `step`** for strict standard-brush parity; calling `step` independently still advances gravity and dynamics.
3. **One-step neighbor propagation delay:** distance bonds were solved before the brush targets, so a 1-iteration/1-substep Grab could move the target particle without propagating to neighbors until the next step. Fixed by applying targets before material bonds; regression test added.

All existing plus three new scenarios pass locally in Release and Debug ASan/UBSan builds. **No Windows/MSVC or in-Blender tests yet.**

## Structural performance limits and prioritized remedies

| Priority | Existing limit | Proposed remedy | Validation |
| --- | --- | --- | --- |
| P0 | All bonds and all particles processed for every step, even brush-local edits | Active particle/bond islands with a graph-radius *halo*, only when there are no global forces/contact dependencies requiring the whole world | Compare local vs global solution error at increasing halo; p50/p95 and missed updates |
| P0 | Entire particle list scanned each dab to select Grab targets | Uniform spatial hash/grid, maintained across deformation; share with future contact broadphase | Brush-selection timing for fixed radius vs particle count |
| P0 | Serial Gauss-Seidel constraint loop | Evaluate Blender's existing `source/blender/geometry/xpbd/GEO_xpbd_constraint_coloring.hh` and `GEO_xpbd_updater_gauss_seidel.hh` to parallelize independent constraints | Wall time + residual error vs thread count; no concurrent writes |
| P1 | Full `World` particle + bond copies in every `begin_stroke()` | Sparse per-stroke journal keyed by stable IDs and per-block modified metadata; maintain exact rollback for breakage/plasticity | Undo exactness and snapshot time/memory at 125k and 1M particles |
| P1 | Repeated square roots and vector-of-struct particles/bonds | Profile first, then try SoA/hot-only arrays, contiguous constraints, compact 32-bit indices; compare float32/mixed precision to double | Numeric error vs double reference; CPU SIMD/GPU coalescing |
| P1 | Multiple temporary arrays and O(N) dirty scan | Track per-step modifications in active blocks; batch PBVH updates; cache scratch | Dirty correctness and wall time with sparse/local dabs |
| P2 | Uniform fixed iterations are sometimes insufficient and sometimes wasteful | Residual-based adaptive substeps/iterations and warm-start where mathematically safe | Stiffness error, stability, temporal consistency |
| P2 | No robust particle contacts or collision topology | Broadphase + narrowphase and contact constraints, friction and self-collision | Stacked granular pile, interpenetration and resting friction benchmarks |

**Important:** A 60 FPS frame has 16.7 ms total, not 16.7 ms entirely for physics. A provisional budget for a sculpt brush may be ~3–5 ms p95 for the physics part, to be revised after profiling a real Blender build. A 27k-particle **global** step currently cannot fit that budget. The first route to useful interaction is a local 2–10k-particle proxy, region updates and low iteration counts; GPU is not the first prerequisite.

## Physical fidelity limitations

- Nearest-neighbor **distance springs are not sufficient** for realistic clay or muscle. There are no volume/incompressibility, shear, bend/torsion or corotational constraints, and no measured volume preservation. Plasticity is rest-length creep, not a calibrated elastoplastic material law.
- `PhysicalAssist` and `FullPhysics` currently share the *same mathematical Grab target routine*: their artistic semantics and blending against the **real Blender brush displacement** do not yet exist.
- Fracture is a threshold on per-bond current extension, before material convergence, and can vary with iteration order/substep size; no crack surface, contact handling or stable fracture propagation.
- Gravity and contact are basic, with a flat floor only. No particle-particle collision or friction; a convincing sand/mud simulation is **not** yet present.
- Surface anchors preserve per-corner UV numerically for unchanged source anchors, but this does **not** guarantee low texture distortion after large deformations or provide UVs on newly exposed fractured interiors.

## Multires Mask Extract fidelity limitations

The reference `mask_extract_core.cc` clips **supplied triangles** and keeps corner-domain UV/color/material values. It does not obtain high-resolution masks from CCG, reconstruct an editable Multires hierarchy, restore multiple arbitrary Blender attributes/UDIM maps, or handle all non-manifold and near-degenerate cut cases. Next: extract real evaluated Multires samples and source-corner provenance, then test seam/Face Set/Polypaint/undo cases before adding smoothing or editable hierarchy transfer.

## Next implementation gates

1. Integrate the existing Blender XPBD utilities through a tiny adapter rather than permanently maintaining duplicate math.
2. Write **local active-island prototype + spatial grid** and compare solutions to the global reference with error bounds.
3. Implement true standard-brush early bypass, `PhysicalAssist` blend semantics, stable dab targets, then Multires active-layer writeback and Blender Undo.
4. Validate a Blender-attached 5k–10k proxy with profiling of solver, displacement transfer, PBVH redraw and stroke lag; **only then** consider CUDA/Vulkan/Metal accelerated paths.
5. Separate high-fidelity Multires Mask Extract as its own small native integration task and keep the source object untouched.

**Status: prototype improvements and regression checks passed; benchmarks preliminary; no claims of Blender integration, speedup on GPU, or production-grade material accuracy.**
