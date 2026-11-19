# Adaptive local XPBD: transactional halo, quality modes, measurements

**18 November 2026 — EXPERIMENTAL C++17 ONLY. Not integrated in Blender.**

## Why it is needed

A fixed bond-hop island with pinned exterior is fast, but may create false stiffness and positional error. Previous standalone tests already showed substantially larger local-vs-global error at four hops than eight hops in one material/scene. The boundary pin is **numerical**, not a real physical constraint.

The new `World::step_local_adaptive()` is a conservative candidate procedure:

1. BFS through **unbroken material bonds** from the current Grab target(s) to a maximum allowed hop radius.
2. Try an initial halo. Snapshot **only active particles, touching bonds**, Grab constraint multipliers, dirty IDs and step statistics before the attempt.
3. Measure displacement of the outermost active particles **that touch unsimulated movable particles**. This distinguishes an artificial interface from legitimate physical pins or isolated components.
4. If shell motion exceeds `max(1e-9, boundary_fraction * max_target_displacement)`, roll back the candidate and retry with a larger halo (`4 → 8 → 16`, etc.).
5. At the cap, choose an explicit policy:
   - **StrictGlobalFallback (default)**: restore rejected local state, run global XPBD, return `used_global_fallback = true`.
   - **BoundedApproximation**: keep the capped local state, return `converged = false`; must not be presented as a globally accurate solution.
6. Store the last successful/capped halo width **within the same stroke** to avoid repeated failed small-halo passes. Reset on begin/commit/cancel.

Both modes include step-level recovery on numerical failures; the global fallback also snapshots the full state to restore on exceptions. The standalone implementation is not an Out-of-Memory recovery guarantee.

## API

```cpp
auto result = world.step_local_adaptive(
    1.0 / 30.0, 4, 5, 4, 8, 0.02,
    World::BoundaryPolicy::BoundedApproximation);
if (!result.converged) {
  // Do not silently represent this as a high-accuracy solve.
}
```

`converged` is based on a **boundary-motion heuristic only**. It is **not a mathematically proven bound on the difference from the global solution** and does not validate clay constitutive physics. Full-scene forces and ground collision still require global solve.

## Local tests

Standalone `test_adaptive.cc`: expansion for a disturbed boundary, intra-stroke halo-width reuse, strict rollback and equality with a direct global solve (particle positions and material bond states), bounded mode equality to the final accepted fixed-hop candidate, numerical overflow rollback. Existing tests for ordinary Sculpt bridge and Multires proxy remain.

On the local development container:

- GCC Release: **4/4 CTest suites pass**.
- GCC Debug with AddressSanitizer and UndefinedBehaviorSanitizer: **4/4 pass**.
- Clang Release: **4/4 pass**.
- Windows/MSVC, exact GitHub CI, in-Blender PBVH, regular Sculpt editor callbacks and Multires grid writeback **not verified**.

### Reproduce

```sh
cmake -S experimental/sculpt_physics_cpu -B build/sculpt-physics \
  -DCMAKE_BUILD_TYPE=Release -DSCULPT_PHYSICS_BUILD_BENCHMARKS=ON
cmake --build build/sculpt-physics --config Release
ctest --test-dir build/sculpt-physics --output-on-failure
build/sculpt-physics/bench_adaptive_physics
```

## Preliminary speed trade-off

Median of five timed runs following one warmup on a virtualized Linux CPU, one thread. Cubic bonded lattices, spacing 0.075, radius 0.2, 0.1-unit Grab, compliance `1e-5`, four substeps × five iterations. **Times below cover solver step only, not full Blender brush latency.** Benchmark resets the stroke for each measurement, so it **does not measure benefit from intra-stroke halo caching**.

| Particles | Fixed 4-hop | Fixed 8-hop | Adaptive bounded, cap 8 | Adaptive strict, cap 8 | Adaptive strict, cap 16 | Global |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8,000 | ~0.95 ms | ~3.7 ms | ~8 ms | ~14 ms | ~19–23 ms | ~8 ms |
| 27,000 | ~1.1 ms | ~3.9 ms | ~7 ms | ~33 ms | ~27–31 ms | ~29 ms |
| 125,000 | ~1.1 ms | ~3.9 ms | ~6.6 ms | ~145 ms | ~33–44 ms | ~141 ms |

**These are preliminary representative runs from the local implementation, not CI-certified numbers.** Times vary between runs and virtualized hosts; do not conclude that adaptive selection itself is faster than a fixed-hop solve. With strict fallback at cap 8, the solver can be **slower than simply choosing global** because of rejected local attempts.

## What this tells us

- The CPU solver localizes computation well, but a strict global-error policy may blow the interactive latency budget.
- For regular Sculpt and Multires, use the same CPU physics core and an explicit **quality/latency choice**, never silently lose precision.
- `BoundedApproximation` is a research mode. Its `converged = false` flag must be visible to the future editor adapter; no automatic fake success. A production editor may choose to defer physical refinement, but that requires a new design and tests.
- The present boundary metric is only a heuristic. Compare **multi-dab histories** and direct global results for a range of stiffnesses, plasticity/fracture settings, and selected regions. Track max/RMS sample error, bond rest length, broken bonds and UV-surface correspondence.
- Initial halo size and `boundary_fraction` must be calibrated per material and scene scale. Do not hardcode 4/8 hops as universal.
- The adaptive implementation still uses a full `distance[N]` array each dab, BFS to `max_hops`, fresh vector allocations, and global full-stroke Undo snapshots. Future speedup: cached topology generation / scratch buffers / sparse journal. The bridge still needs dirty-tile writeback and real Blender Undo integration.

## Status and release gate

This experimental branch is standalone C++ reference code only. It is **not** compiled into the released Blender build and has no native Grab physical brush yet.

Before any merge into production: (a) exact GitHub branch builds in Linux and Windows CI, (b) multi-stroke differential accuracy tests, (c) benchmark complete Sculpt callback + proxy transfer + PBVH/Undo, (d) fail-safe behavior when local precision is not met, (e) no regression when physics is OFF.

**Sources:** `adaptive_solver.cc`, `test_adaptive.cc`, `bench_adaptive_physics.cc` and the earlier `LOCAL_SCULPT_ADAPTER_AUDIT_20261118.md`.
