# Motion continuity across Sculpt dabs: CPU XPBD V4 audit
**18 November 2026 | Experimental standalone C++ reference — not integrated into Blender.**

## Confirmed correctness problem

`World::step_local()` only integrated the brush-selected bond island. It left all other particles stationary even if they had **nonzero velocity retained from earlier dabs or previous strokes**. An artist brushing a second, disconnected region could unintentionally freeze a still-moving earlier region; the result diverged from global dynamics without signaling a fallback.

### Fix: deterministic dynamic-safety gate

1. The local solver checks particles with nonzero stored velocity that lie **outside** the current active bond-hop island. If their expected position change over the requested `dt` exceeds `1e-10` units (maximum absolute velocity component), it runs a **global XPBD step**, with particle/bond/target/dirty/registry rollback if the step throws.
2. `step_local_adaptive()` includes previously moving particles **reachable within the allowed maximum halo** by raising the starting halo width; if any moving component is disconnected or beyond that budget, it performs the existing transactional global fallback.
3. A new **sparse moving-particle registry** records indices after successful global/local steps. This avoids a new `O(N)` scan of every stationary proxy particle on each dab. It is preserved across committed strokes and restored on stroke cancel and rejected adaptive trial.
4. `targets_.empty()` with no gravity/contact remains an explicitly quasistatic **no-op**. For continuous scene dynamics without a brush, call the global simulation path intentionally; do not confuse no-target idle behavior with paused animation.

### Tests

New `test_motion_consistency.cc` exercises:
- Two separate, sequential strokes on disconnected geometry: local and adaptive both fall back and **match direct global integration** of both prior moving and current target particles.
- Two targets on the same bonded component: previously moving particles are included in the adaptive region without unnecessary global fallback when `max_hops` allows it.
- Empty/no-target local dab remains idle.
- Exceptional global fallback restores pre-step particle positions/velocities instead of leaving a corrupt partially stepped state.

Existing tests remain intact, including regular mesh Sculpt surface bridge, Multires sample bridge, UV seams, halo boundary reaction, fracture/plasticity rollback and mask extraction.

### Local verification

Same standalone source directory, Linux development container:
- GCC C++17 Release: **5/5 CTest suites passed**.
- Clang C++17 Release: **5/5 passed**.
- GCC Debug ASan/UBSan: **5/5 passed**.
- Windows/MSVC: **requires exact latest GitHub Actions CI result**.
- Blender native compile, viewport Brush/PBVH, real Multires Sculpt Layers and Undo: **not implemented or tested**.

## Performance impact and limitations

The straightforward global safety scan initially increased fixed 4-hop local solver time on a 125k-particle lattice (about 1.0 → 1.8 ms in one noisy test pair). Tracking sparse moving IDs recovered most of this: an updated run measured **~1.0 ms** for the 125k/4-hop case and ~4.9 ms for 8-hop, excluding `begin_stroke`, mask/UV transfer, PBVH and rendering. Runs were not hardware-pinned and are **not evidence of a statistically significant speedup**.

The sparse registry may contain many entries after a global dynamic solve. A truly dynamic scene may therefore still require global stepping, and the fallback can cost hundreds of milliseconds with many particles. This is an intentional correctness-first policy, not a general real-time dynamics solution.

**Remaining optimizations:** spatial brush lookup, active moving-component simulation rather than whole-world fallback, sparse Undo journal, reusable graph scratch and multiscale proxy for dense Sculpt. A small but nonzero velocity threshold may ignore sub-visible displacement; test scale and tolerance when Blender real-world object units are available.

## Reproduce

```sh
cmake -S experimental/sculpt_physics_cpu -B build/sculpt-physics -DCMAKE_BUILD_TYPE=Release \
  -DSCULPT_PHYSICS_BUILD_BENCHMARKS=ON
cmake --build build/sculpt-physics --parallel
ctest --test-dir build/sculpt-physics --output-on-failure
build/sculpt-physics/bench_local_physics
```

**Status: CPU reference only, experimental PR #21 remains Draft.**
