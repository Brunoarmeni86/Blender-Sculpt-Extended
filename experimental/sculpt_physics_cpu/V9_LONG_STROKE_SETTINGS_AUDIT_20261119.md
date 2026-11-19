# V9 long-stroke regression and configuration transaction audit

**19 November 2026 — Experimental standalone C++17 CPU prototype; Draft PR #21. NOT a Blender-integrated feature.**

## Confirmed bug: physical configuration leaked across cancelled strokes

`World::cancel_stroke()` restored the particle/bond geometry, velocities and moving indices but **did not restore world configuration** changed during a stroke. A user who changed gravity, damping or ground enabled/height while a stroke was active could cancel the stroke, only to find the next brush dab still influenced by the cancelled changes.

**Minimal reproducer:**

```cpp
World w;
w.add_particle({0,1,0});
w.begin_stroke();
w.set_gravity({0,-9.81,0});
w.set_velocity_damping(17.0);
w.set_ground_height(10.0);
w.cancel_stroke();
w.begin_stroke();
w.apply_grab({0,1,0},.2,{0,.1,0},{BrushPhysicsMode::FullPhysics,1,0});
w.step_local(1.0/30.0,1,1,0);
// Before fix: used_global_fallback=true due to leaked gravity/contact.
// After fix: local step remains local.
```

**Fix:** save `gravity_`, `damping_`, `ground_enabled_` and `ground_y_` in `begin_stroke()`; restore exactly in `cancel_stroke()`. `commit_stroke()` deliberately retains changed settings, so a subsequent cancelled stroke restores that *committed baseline*. Adds four scalar/Vec3 fields per World; no new per-dab heap allocation.

The transaction is still **standalone** and does not implement Blender property Undo/Redo, which will need native editor integration and explicit ownership of brush settings vs project-wide settings.

## Extended deterministic regression tests

Added `test_v9_resilience.cc`, the **eleventh** standalone CTest suite in the V8 branch. It checks:

1. **640 alternating sculpt strokes** (4 XPBD distance-bond compliance values: `0`, `1e-10`, `1e-5`, `1e-2` × 160 strokes), comparing ordinary global stepping to multi-island local stepping across two fully selected and disconnected three-particle components. At every dab, compare positions, velocities and bond material state. Every fourth stroke is cancelled and compared with its pre-stroke copy.
2. **80 additional alternating plastic/fracture material strokes**, with pinned particles, nonlinear rest-length creep and breakable bonds. Global and multi-island states are compared after each step, including after repeated cancel/commit cycles.
3. **Exact config cancellation:** alter gravity, damping and floor during a stroke, then cancel and verify next local Grab does not fall back globally.
4. **Committed config baseline:** commit gravity/damping/floor settings, overwrite them in another stroke and cancel, then verify the committed gravity/contact state remains active.

**What this validates:** the multi-island path produces the same result as the global solver **for these fully covered, disconnected components and these material profiles**, and the configuration transaction is exact. It does **not** prove equality when active islands cut through a connected mesh, or calibrate clay material laws. This deliberately isolates incremental-integration correctness from artificial-boundary approximation.

## Local checks and CI

A local V7-derived C++17 baseline with the V9 configuration fix and tests passed:
- GCC Release: 10/10 local suites
- Clang Release: 10/10
- GCC Debug ASan/UBSan: 10/10

The GitHub V8 branch already has one additional V8 suite and therefore runs **11 CTest suites** with V9. Check the exact final GitHub Actions run for Linux, Windows/MSVC and sanitizers; don't claim Windows verified until the workflow completes.

## Remaining high-priority work

- The local vs global equivalence shown here only applies when **entire connected material components** are selected for simulation. Artificial halo boundaries still need an adaptive *reaction-based* tolerance and global differential tests.
- `apply_grab` still performs an O(N) spatial scan, and `step_local_adaptive` still allocates graph/BFS arrays every call.
- Sparse first-touch Undo in `World` is not integrated into Blender Sculpt Undo or PBVH/Multires CCG layer writes.
- No volume/shear constraints, particle contact/friction, realistic elastic/plastic clay calibration, or high-resolution displacement conservation tests in Blender.
- Future benchmarks should use large **connected tetrahedral or surface meshes**, not only two 3-particle islands, with p50/p95 for the complete brush callback including selection, solver, PBVH and proxy transfer.

**Keep Draft PR #21; production Blender build remains unchanged.**
