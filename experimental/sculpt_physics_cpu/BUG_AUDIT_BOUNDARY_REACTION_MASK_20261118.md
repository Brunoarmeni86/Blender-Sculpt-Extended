# Bug hunt: False Adaptive Convergence, Mask Extract Degeneracy and CPU Hot Paths
**18 November 2026 — experimental standalone C++ only. No Blender integration.**

## Confirmed high-severity bug: false convergence on a stiff artificial boundary

**Reproducer:** two particles at x=0 and x=1, connected with an incompressible distance bond; Grab x=0 by +0.25; local halo = 0; 10 iterations; a one-step global solve produces x≈0.249756 on the target, but the original adaptive solver produced **x=0** and returned **`converged=true`**.

**Root cause:** the adaptive error gate only inspected **how far the boundary moved**. A stiff XPBD distance constraint with an artificially pinned exterior can exactly cancel the brush target and leave the boundary position unchanged. Zero motion does **not** imply zero numerical boundary reaction. This can manifest as a weak or apparently broken Grab/Clay brush.

**Fix:** add a second signal for *crossing, unbroken bonds with movable exterior neighbors*: `max_boundary_reaction = max(abs(bond.lambda) * active_particle.inverse_mass)`. Accept an artificially bounded local solve only if **both** boundary motion and this reaction are below the threshold. The reaction is expressed in approximate position-correction units; it is a heuristic, not a certified global error bound.

**After fix:** strict mode correctly rejects the zero-hop local approximation and falls back to global (x≈0.249756); bounded mode reports `converged=false`. Added a precise regression test covering both policies, with reaction >0.2 and motion ~0.

**Trade-off:** this stronger criterion is deliberately more conservative, so it can enlarge the halo or trigger expensive global fallbacks more frequently. Benchmark strict latency and fallback frequency before editor integration. Last-substep `lambda` is a practical but imperfect reaction proxy; consider tracking maximum boundary reaction over all substeps in a future numeric-quality pass.

## Confirmed geometry bug: degenerate Mask Extract output

**Reproducer:** all-mask-inside source triangle with collinear vertices, or repeated vertex IDs. The previous routine emitted zero-area triangles, potentially producing malformed geometry for later Solidify/remesh/UV transfer.

**Fix:** reject near-zero-area emitted triangles **before** creating output vertices, using a relative area-vs-edge-length check. Added tests for both collinear and repeated source corners. The reference is not a production-ready non-manifold validator.

**Optimization:** the clipping routine allocated `std::vector<Corner>` buffers twice **per input triangle**. For clipping a triangle against one scalar threshold, at most 4 vertices result; replace with two small fixed-size `std::array<Corner,6>` scratch buffers and index counters, avoiding allocations on the normal path.

A quick one-host, **not statistically robust** microbenchmark on 120,000 independent clipped triangles gave median ~175ms old vs ~164ms modified (5 samples after warmup); virtualized CPU timings varied significantly. Do **not** treat this as a guaranteed speedup; rerun on the target workstation and profile allocations/peak RSS.

## Local validation

- GCC Release — 4/4 CTest suites pass.
- GCC Debug with AddressSanitizer + UndefinedBehaviorSanitizer — 4/4 pass.
- Clang Release — 4/4 pass.
- A regression previously failed semantically without this fix (local x=0, falsely `converged=true`); now matches global by fallback.
- GitHub CI branch / Windows status must be verified separately from these local checks.
- No native Blender project build, PBVH, Sculpt Layers, Undo transaction or Multires SubdivCCG extraction was tested.

## Still-open correctness risks

1. **Inertia outside local island:** the local solver is advertised as quasistatic, but integrates nonzero velocity inside the island while leaving previously moving particles outside frozen. Explicitly define stationary/proxy-only behavior, or trigger a global/full-component advance when persistent inertia matters; test alternating distant dabs.
2. **No proven global-error bound:** boundary motion and multiplier reaction catch the stiff-pin counterexample, but may still miss errors accumulated over several substeps or multiple brush dabs. Run local-vs-global comparison over repeated strokes, varying compliance, plasticity and fracture.
3. **Error atomicity:** `step_local_adaptive` catches failed local trials, but raw `step()` and `step_local()` can leave partially mutated state after numerical exceptions. The future Blender adapter must use transactions and writeback only on successful completion.
4. **Surface transport:** Multires proxy triangles may invert or shear; current tangent-frame transport ensures rotation for valid triangles but doesn't certify volume, orientation or texture distortion.
5. **Mask Extract:** non-manifold source, attribute transfer beyond one UV/color layer, near-threshold slivers and SubdivCCG mask ownership still need dedicated testing.

## Remaining optimization opportunities ranked by likely value

| Priority | Optimization | Why | Proof needed |
| --- | --- | --- | --- |
| P0 | **Persistent local graph scratch**: generation-stamped visitation, cached adjacency/halo and preallocated bond lists | Avoid full `distance[N]` initialization, sorting and fresh allocations per dab | End-to-end p50/p95 vs local island size, memory profile |
| P0 | **Spatial brush selection** via uniform hash or BVH | `apply_grab` still scans all particles; high-density normal Sculpt needs a much smaller proxy | Query benchmarks, rebuild cost during deformation |
| P0 | **Sparse Undo journal** with exact particle/bond first-write capture | `begin_stroke` still copies every particle and bond even for tiny dabs | Stress and rollback tests at 100k–1M |
| P0 | **Blender-native bridge** with no-op Standard and 0% Assist paths | No real editor brush/Undo/PBVH/Multires integration yet | Build and visual tests on regular Sculpt and Multires |
| P1 | Constraint coloring and CPU threading | Current solver is serial Gauss–Seidel | 1/2/4/8 core scaling with deterministic tolerances |
| P1 | Adaptive reaction measured across every substep, and per-material thresholds | Last-substep lambda can underestimate transient boundary force | Repeated stroke differential tests |
| P1 | Region-limited Multires sample transport | Proxy frame recomputed for each high-res sample; dense arrays costly | Dirty-tile latency and high-frequency detail error |
| P2 | Volume/shape constraints, collision broadphase, friction | Springs alone do not model realistic clay or granular materials | Constitutive and contact benchmarks |

**Release policy:** keep PR #21 Draft; do not claim these fixes are present in the distributed Blender build.
