# Sculpt Physics CPU — additional optimization research and adversarial tests
**18 November 2026 — research and standalone reference code only. Not integrated into Blender.**

## Newly verified issues and regression fixes

| Finding | Reproducer | Fix on experimental branch | Remaining risk |
| --- | --- | --- | --- |
| Irreversible plastic damage across an artificially pinned local halo | Two bonded particles, zero-hop island, grab stretches the boundary bond; `rest_length` was modified | `step_local` now permits plastic/fracture updates only when **both bond endpoints are active**. Boundary bonds still provide elastic positional support | Actual fracture near the boundary must trigger a larger island or full solve; freezing damage is a safety policy, not a physically exact result |
| Irreversible fracture across the same boundary | Two-particle zero-hop island with small fracture strain; bond broke due to artificial fixed endpoint | Same crossing-boundary guard, plus a positive regression showing **internal** bonds may still break | Adaptive halo is required |
| Non-finite texture data in Mask Extract | NaN per-corner U coordinate was silently propagated into output triangles | Check finite positions, UV and RGBA before emitting geometry. UDIM coordinates beyond [0,1] and HDR colors remain valid | Polygon degeneracy, non-manifold topology, CCG HR attributes still untested |
| Normal-Sculpt proxy alignment false negative at very large translations | Regular mesh at x=1e12; proxy displaced +100 units was accepted because tolerance scaled with absolute coordinate | Tolerance is based on **object extent**, capped to 1e-3 units; distance measured with `std::hypot` | Tolerance must be revisited for extreme object-space scales and actual Blender float/CCG pipelines |
| Potential infinite bond rest length | Finite ±1e308 particle positions make `length(p1-p0)` overflow | Robust `std::hypot` in **bond creation only**, reject non-finite rest lengths | Other extreme runtime numerical overflows still possible |

The numerical **hot bond iteration retains its existing fast distance calculation**; switching every constraint to `std::hypot` risks regressing brush latency. Accuracy and stability must be measured before changing hot arithmetic.

New `test_adversarial.cc` added to CTest (three suites including previous tests). A locally modified copy passed the original 2/2 CTest suites plus the new adversarial tests in GCC Release, and the new tests passed under GCC ASan/UBSan. These are **not tests of a complete Blender build**; Windows/MSVC and exact CI branch compilation remain pending.

## Additional technical findings from research

### 1. Small Steps XPBD — test solver schedules rather than only iteration count

Reference: Macklin et al., *Small Steps in Physics Simulation* (SCA 2019): https://diglib.eg.org/items/322424d5-a24f-490a-a6bb-542233852032

Test `4×5`, `10×2`, `20×1` on **the same total constraints processed**, measuring trajectory RMS, penetration, plastic yield time and fracture onset. More substeps may improve accuracy but increase integration/neighbor-search overhead. Never optimize on a single unconstrained chain only.

### 2. Coarse-to-fine convergence for stiff clay / muscles

- *A Multi-layer Solver for XPBD* (SCA 2024): https://diglib.eg.org/items/22617176-99e5-4bf7-a6d4-63e74cdcebfc
- *MGPBD: A Multigrid Accelerated Global XPBD Solver* (SIGGRAPH 2025): https://github.com/chunleili/mgpbd

Both address slow low-frequency propagation in local iterative solvers. Propose a lightweight **coarse physical deformation mode** for Sculpt, with local fine corrections: no need to simulate a 40M-vertex Multires surface. Research implementations require license, dependency and portability review before copying code.

### 3. Threaded constraint coloring — first reuse Blender core

Blender source already provides `source/blender/geometry/xpbd/GEO_xpbd_constraint_coloring.hh`, `GEO_xpbd_updater_gauss_seidel.hh`, and `xpbd_constraint_coloring.cc`, plus related parallel solver code. Precompute colors for stable material topology; recalculate/downgrade only affected partitions on fracture.

Benchmark **1, 2, 4, 8 worker threads** against serial solve with tolerance-aware output comparisons. Parallel updates to shared particles without graph coloring or an appropriate Jacobi accumulation are invalid.

Blender XPBD documentation: https://developer.blender.org/docs/features/nodes/xpbd_solver/

### 4. Spatial index + persistence — make locality persistent

- An ordinary Sculpt Grab currently scans all N particles to collect targets; replace with a **uniform spatial grid** or BVH query around the brush. Maintain/rebuild when particles cross cell boundaries.
- Reference for hardware-independent algorithm: https://nvidia.github.io/warp/v1.16/user_guide/runtime.html (Warp HashGrid is reference for the data structure, **not** an AMD GPU backend).
- Reuse allocated `active`, `selected_bonds`, incidence and scratch buffers across dabs; introduce topology generation IDs for safe cached islands. Avoid sorting full bonds; sort only the local set when deterministic order is necessary.
- Keep one **sparse undo journal per stroke**, recording particles, bond rest lengths, lambda and broken states on first write only. On cancel, explicitly mark all restored PBVH/CCG regions dirty.
- Record time for **snapshot + brush selection + island construction + solver + surface bridge + PBVH upload**. Solver-only medians do not represent interaction latency.

### 5. Adaptive halo and boundary residual

The existing fixed-hop local solver can yield false stiffness and errors near the cut. A proposed robust algorithm:
1. Select seed particles from brush; include a deformable interior and a **buffer halo** through unbroken bonds.
2. Keep the *outermost shell* constrained as a numerical interface; never modify plastic/fracture states on shell-crossing bonds.
3. Estimate boundary reaction/error: max positional corrections and XPBD residual on shell bonds, compared with the local material scale and brush displacement.
4. If above tolerance, **rollback only that dab** and solve again with a wider halo. Reuse the previous accepted geometry as the authoritative state.
5. For colliders, large impulses, unconverged stiffness, full-body movement or unknown interactions, fall back to an explicit global simulation.

For visual accuracy, compare local-vs-global after **multi-dab strokes, alternating directions and release** (not just a single dab). Measure differences in rest lengths, broken bonds and UV surface locations as well as particle positions.

### 6. Better material fidelity / preservation

Distance springs alone are not enough for real clay or tissue. Evaluate continuum volume/shape energy such as tetrahedral XPBD Neo-Hookean constraints and incompressibility/volume conservation, then implement explicit plastic yield criteria, contact/friction, self-collision and reliable surface provenance.

Reference C++ XPBD constraints (including tetrahedral volume): https://github.com/InteractiveComputerGraphics/PositionBasedDynamics/blob/master/PositionBasedDynamics/XPBD.h

**Normal Sculpt, without Multires, must also support a coarse physical proxy**: 1:1 particles per mesh vertex is too expensive on dense meshes. Multires then uses the same proxy/anchor infrastructure but writes active Sculpt Layer deltas to CCG. Preserve per-corner UV and colors across both paths. Fracture changing topology should stay separate from live Multires layers initially.

### 7. Future GPU and portability

The **CPU reference remains the canonical fallback**. Compare GPU options only after measurable CPU correctness; graphical Vulkan support does not imply a ready general compute solver. A Vulkan C++ XPBD reference exists (https://github.com/20niship/nebula); it is not a drop-in Blender module and must be independently audited for license, quality and hardware compatibility. NVIDIA Warp is primarily a CPU/CUDA development option, not a universal GPU backend.

## Latest repeatable local data (same test host class; do not treat as hardware guarantees)

Single-thread standalone C++17 optimized CPU, cubic particle lattice, 4 substeps × 5 iterations, one warmup + 5 measured runs, medians:

| Particles | Global solver | Local 4-hop solver | Local 8-hop solver |
| ---: | ---: | ---: | ---: |
| 8,000 | 7.662 ms | 1.055 ms | 3.639 ms |
| 27,000 | 24.979 ms | 0.964 ms | 3.282 ms |
| 125,000 | 122.449 ms | 0.982 ms | 3.716 ms |

No Blender PBVH/viewport/Undo/transfers in these timings. Differences from earlier runs reflect variable test hosts/build conditions, **not evidence that these corrections accelerated the solver**. Accuracy for fixed-hop regions is still unbounded outside existing single-dab tests.

## Engineering gate before native Sculpt attachment

- [x] Verify and fix artificial halo plastic/fracture corruption in standalone reference.
- [x] Validate finite output geometry attributes and reject unsafe proxy translation.
- [x] Add automated adversarial regressions in experimental branch.
- [ ] CI compile and run all 3 tests from exact current branch on Linux and Windows/MSVC.
- [ ] Adaptive halo and per-dab checkpoint; verify tolerance over multiple strokes.
- [ ] Implement sparse rollback journal and invalidation on cancel.
- [ ] Profile real normal Sculpt and Multires dirty-tile bridge, then consider accelerated backends.

**Keep the PR Draft. This is a CPU reference audit, not shipping Sculpt physics.**

## Small Steps experiment — result from this reference solver

Reproducible `bench_substeps` target (enable `SCULPT_PHYSICS_BUILD_BENCHMARKS=ON`).
It compares equal **20 total constraint iterations** for `4x5`, `10x2`,
`20x1`, plus an unequal `40x2` high-substep diagnostic. One warm solver
run per configuration; this is a preliminary **single run**, not p50/p95.

| Grid | Schedule | Solver time | RMS relative bond length error | Maximum relative bond error |
| --- | --- | ---: | ---: | ---: |
| 1k particles | 4x5 | 1.063 ms | 0.01244 | 0.3480 |
| 1k particles | 10x2 | 0.980 ms | 0.01381 | 0.3090 |
| 1k particles | 20x1 | 0.974 ms | 0.01626 | 0.2974 |
| 8k particles | 4x5 | 10.198 ms | 0.00530 | 0.3648 |
| 8k particles | 10x2 | 12.593 ms | 0.00535 | 0.3492 |
| 8k particles | 20x1 | 9.709 ms | 0.00595 | 0.3367 |

Interpretation: more substeps decreased **the maximum** relative length violation
in this setup, but did **not** reduce the RMS of all bond-length deviations.
The simple metric is not a ground-truth measure of physical accuracy or visual
quality and can be influenced by legitimate compliant deformation. Repeat
with volume conservation, multi-dab histories, collisions and calibrated
references before choosing an adaptive substep policy.

