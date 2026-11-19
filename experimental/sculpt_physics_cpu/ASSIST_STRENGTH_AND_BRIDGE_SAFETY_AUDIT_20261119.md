# CPU Sculpt Physics — Assist strength and bridge safety audit

**Date:** 19 November 2026 · **Scope:** experimental CPU reference branch / PR #21.
**Not Blender-integrated.** No production UI or native PBVH / CCG writeback is implied.

## Confirmed bug A — double application of Assist influence

`World::apply_grab_impl` previously computed the target falloff as
`falloff * influence`. `SculptSurfaceBridge::compose_positions` then
interpolated again with `influence`. At the brush center, an isolated particle
with zero compliance and zero ordinary-brush displacement therefore received
25% of a requested displacement when `influence=0.5`, rather than 50%.

**Fix:** in `PhysicalAssist`, the physics Grab target receives the complete
falloff, while the surface bridge applies the requested Assist strength exactly
once. `FullPhysics` continues to scale the target directly. The 0%-Assist
and Standard-mode bypass remain unchanged.

**Regression test:** `test_assist_strength.cc` covers 25%, 50%, 75%, 100%
Assist, both full-scan and PBVH-candidate paths, FullPhysics at 50%, and the
Standard / 0% bypass. Registered in CTest as `sculpt_assist_strength_tests`.

## Confirmed bug B — non-finite normal Sculpt Assist output

The Multires bridge already validated a blended output, but the regular Sculpt
path could return `NaN` when the caller supplied non-finite artistic brush
positions. This is unsafe for a future native mesh writeback.

**Fix:** regular Sculpt physical-assist output now validates the blended
position and throws `std::invalid_argument` for non-finite results. A new
regression passes a NaN ordinary-brush input. The explicit Standard/0% bypass
continues returning the untouched ordinary geometry.

## What was already implemented on the latest GitHub branch

The branch had advanced significantly beyond the earlier locally exported
ZIP: sparse stroke Undo with dense fallback, generation-stamped scratch,
PBVH candidate filtering, artificial boundary protection, adaptive halo,
multi-island stepping, numeric exception atomicity, and extensive additional
CTest suites are **already in GitHub**. Do not replace them with the earlier
local prototype or double-count them as new features.

## Evidence and limitations

- A standalone *older local source snapshot*, with equivalent corrections
  applied, passed 3/3 CTest suites in GCC Release, GCC Debug ASan/UBSan, and
  Clang Release; this did **not** compile the complete current GitHub branch.
- An isolated 125k particle / 367.5k bond local benchmark compared the older
  full-copy Undo approach against an independent sparse-journal patch:
  median complete local stroke plus cancel: 2.978 ms -> 0.440 ms.
  This is **not** an A/B benchmark of the present GitHub branch; that branch
  already has its own sparse Undo implementation.
- The current GitHub commit showed no associated Actions workflow runs or
  combined status checks when inspected. **The new CTest target must still
  be built and run on the exact branch**, particularly Windows/MSVC.
- These tests use trivial brush targets. They do not validate realistic
  artist strokes, weighted masks, pose/transform parity or a Blender build.

## Next performance targets (unmeasured on the current branch)

1. **Adaptive graph lookup:** `adaptive_solver.cc` still allocates and
   initializes `std::vector<int> distance(particles_.size(), -1)` per dab.
   Replace with persistent distance/visit-generation scratch and benchmark
   125k/1M/10M proxy vertices, because a small brush should not trigger
   an O(N) initialization.
2. **Adaptive retry cost:** each candidate halo constructs particle and bond
   checkpoint vectors, sorts bonds and may run a full failed iteration.
   Investigate scratch reuse, incremental halo expansion and an early
   iteration/quality gate to bound p95 latency.
3. **Graph coloring:** leverage Blender's existing XPBD coloring machinery
   instead of introducing another serial-only solver; prove the resulting
   numerical parity and cost on 1/2/4/8 CPU threads.
4. **Multires writeback:** process only dirty CCG tiles; avoid rebuilding
   local tangent frames for every high-resolution sample per dab.
5. **End-to-end measurement:** p50/p95 total latency including brush
   candidate generation, proxy extraction, per-dab journal, XPBD,
   PBVH updates, Multires Sculpt Layer writeback and viewport redraw.

## Native integration gate

Do not enable by default. First implement a real Grab callback in ordinary
Sculpt Mode with Standard/0% fast bypass and Blender Undo transaction; prove
save/reopen and cancel. Then add CCG/Multires layer delta transfer. Keep all
changes experimental until Blender build and regression tests pass.
