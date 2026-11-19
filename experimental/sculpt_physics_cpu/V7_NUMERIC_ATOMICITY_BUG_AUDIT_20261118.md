# V7 bug audit — Numeric atomicity, Grab validation and pinned-particle safety

**18 November 2026. Experimental standalone C++17 reference in Draft PR #21. Not integrated into Blender.**

## Three confirmed bugs, reproduced and corrected

### 1. Error during XPBD step left a partially modified world

**Reproducer:** one dynamic particle at x=0 and a huge, but finite, Grab displacement 1e308. `step()` raised after computing infinite velocity, yet **the particle remained at x=1e308**; the raw `step_local()` and `step_local_multi()` paths had the same failure mode.

**Fix:** before a full-world step, snapshot the **current** particles, bonds, Grab targets, dirty IDs, moving particle index and stats; on any exception restore via non-throwing vector swaps. For local and multi-island steps, snapshot only **active particles and touching bonds** plus metadata and restore them on error. These are *per-dab* checkpoints, separate from the **pre-stroke hybrid sparse/dense Undo**; an earlier successful dab remains accepted after a later failed dab. The existing `step_local_adaptive()` per-candidate rollback remains.

**Regression:** execute overflow in all three raw step paths; verify unchanged state, including plastic bond length and fracture flag after a previous valid dab; recover with another valid dab; finally `cancel_stroke()` must restore the original stroke.

**Performance cost:** per-step local snapshots add O(active particle + touching bond) copying. Global steps add O(N+M) checkpoint traffic **on top of** the stroke Undo snapshot. This protects correctness but may degrade throughput; use a native editor-side staging transaction or compressed touched-block checkpoints before claiming production speed.

### 2. Grab could accept invalid targets or lose the prior valid dab

**Reproducer:** particle near x=1e308 with finite displacement x=1e308 caused `goal = position + displacement` to overflow, but `apply_grab()` still returned a target. Its old implementation also cleared prior valid targets before finishing validation/building the new list.

**Fix:** construct targets in a reusable **pending target buffer**, validate finite goals and effective compliance, and swap into the active target list only after a complete successful pass. Standard and zero-influence paths still disable targets. For unusually large finite brush radii, use `std::hypot` when squared radius would overflow, avoiding erroneous `inf >= inf` rejection.

**Regression:** invalid target must throw without destroying the prior usable Grab; a brush with finite radius 1e180 must still select a particle at 1e170.

### 3. Tiny timestep silently corrupted even pinned particles

**Reproducer:** two pinned particles with a rigid distance bond, `dt = 1e-200`. In XPBD `alpha = compliance / (h*h)`, where `h*h` underflows to zero; with zero compliance, `0/0` produces NaNs. The global solver previously skipped validating pinned particles, so it could return **success with NaN coordinates and constraint multipliers**.

**Fix:** reject time steps whose squared substep would underflow to zero in both global and local paths. Skip positional correction for inverse-mass-zero endpoints, instead of evaluating `0 * NaN`. Validate pinned as well as dynamic particle state at the end of global substeps. This is numerical safeguarding, not a constitutive-physics accuracy guarantee.

**Regression:** both global pinned-bond and local Grab with extremely small `dt` reject invalid input without changing geometry.

## Verification

New standalone CTest suites: `sculpt_error_atomicity_tests`, `sculpt_grab_validation_tests`.

- Local GCC Release: **9/9 suites passed**.
- Local Clang Release: **9/9 suites passed**.
- Local GCC Debug with ASan and UBSan: **9/9 suites passed**.
- Latest exact-commit GitHub Actions Windows/MSVC status must be checked separately; no Blender binary build was performed.

## Preliminary performance comparison

One virtualized Linux host, 10k/50k/125k particle **bonded chain**, single target, one substep / two iterations. Median local-stroke time from a quick paired run:

| Case | V6 | V7 |
| --- | ---: | ---: |
| 10k local stroke | 0.022 ms | 0.024 ms |
| 50k local stroke | 0.128–0.184 ms | 0.135 ms |
| 125k local stroke | 0.436–0.455 ms | 0.524 ms |

The 125k pair suggests a possible **~15–20% local regression** from the new checkpoints in this microbenchmark, not a speedup. Runs varied with virtualized CPU load; global timings varied even more. Full Blender PBVH/CCG, native Undo, collisions, surface transfer, rendering and GPU synchronization are excluded. Never extrapolate directly to a sculptable high-resolution clay surface.

## Remaining issues and follow-up experiments

1. **Checkpoint optimization**: chunk-based, first-write journal per dab, safe rollback of partial solver work, then release snapshot resources on accepted step; benchmark against the simpler V7 implementation.
2. **Attribute/scene mutation**: `World::set_gravity()`, damping and ground settings can change during a stroke; cancel only restores physical particle/bond state, not these configuration parameters. Define editor transaction ownership for brush settings.
3. **Numerical conditioning**: XPBD compliance/strain extremes, huge radii/displacements, degenerate anchors, triangle flips, volume conservation, highly nonuniform particle masses. Add fuzz tests around finite extremes and material discontinuities.
4. **Undo on the actual Blender mesh**: standalone cancel restores the proxy but still does **not** invalidate PBVH or reconstruct Multires CCG active-layer updates. Do not claim real Blender Undo/Redo.
5. **Collisions/plasticity accuracy**: no particle-particle broadphase, measured clay incompressibility, accurate crack surface or full material law yet. Multires and regular Sculpt are currently independent reference bridges, not native brush hooks.
6. **Concurrency**: reusable scratch, moving registry and Undo maps are owned by a mutable `World`; the solver is **not thread-safe**. Gate parallel constraints with a well-defined execution/ownership model.

**Status:** three reproduced bugs fixed in experimental CPU reference. PR stays Draft, stable Blender release unchanged.
