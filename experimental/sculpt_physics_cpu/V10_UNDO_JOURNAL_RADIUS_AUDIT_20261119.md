# V10 — Failed-dab Undo journal and tiny-radius Grab audit

**19 November 2026 | Experimental standalone C++17 reference, Draft PR #21. NOT integrated into Blender.**

## Confirmed issue 1: Ghost Undo records from numerically rejected local dabs

In V9, `World::step_local_impl()` inserted first-touch particle/bond originals **before** its numerical solve. The V7–V9 per-step exception recovery restored modified physics state but **did not remove Undo entries inserted during the failed attempt**.

Minimal repro: one particle at x=0; begin stroke; finite Grab displacement 1e308; local one-substep/one-iteration solve throws when the calculated velocity becomes non-finite. After rollback, position returns to zero but `stroke_journal_statistics().particles_recorded == 1`, even though **no dab was accepted**.

**Fix:** reusable `local_undo_new_particle_ids_` and `local_undo_new_bond_ids_` lists identify precisely which first-touch entries are added in the current local step. The lists are reserved before the step, and first-touch insertion is performed **inside the existing exception-protected transaction**. If the step fails, remove newly created entries and restore particles, bonds, target state, moving IDs and dirty metadata. Preserve journal originals recorded by earlier accepted dabs.

For performance, use a **single `unordered_map::try_emplace`** call rather than first checking membership and subsequently inserting; reuse capacity of the added-ID vectors. This adds tracking work to local dabs but avoids accumulating Undo garbage after errors.

### Adaptive quality-rejected candidates

A `step_local()` attempt can be numerically valid and still be **rejected** because the adaptive boundary quality gate requires a wider halo. The outer adaptive checkpoint now also tracks which first-touch Undo entries a candidate created; `restore()` erases only those candidate additions before retry or strict global fallback.

The new test specifically builds a stiff artificial boundary with a **distant high-compliance bond**: the local candidate is rejected, and the attempted global fallback throws on non-finite scaled compliance. The stroke Undo journal must still contain **zero** particle/bond records with `dense_fallback=false`.

## Confirmed issue 2: Underflowing squared Grab radius missed an exact-center hit

With `radius = 1e-200`, `radius*radius` underflows to zero, so the existing fast path `dist_sq >= radius_sq` returns `true` even at the exact center of the brush. `apply_grab()` incorrectly returns `false`.

**Fix:** retain the fast squared-distance path only if `radius_sq` is finite **and strictly positive**; otherwise evaluate the distance using overflow/underflow-resistant `std::hypot(x,y,z)`. This safeguards both extremely large and extremely small positive radii while preserving the ordinary fast path. Such radii are a numeric robustness test, not a realistic Blender brush setting.

## Added regression coverage

New `test_v10_journal_radius.cc` (12th suite on the V9 branch):
1. Tiny positive radius hits a particle at the exact center, with no geometry edit until stepped.
2. Numerically rejected local dab leaves **no first-touch Undo records**, then a valid dab succeeds and stroke cancel restores original state.
3. Previously accepted local edit survives a failed dab in a different moving island; journal preserves only the accepted originals.
4. Rejected adaptive halo plus failing global fallback leaves **no ghost Undo entries** and no geometry change.

### Validation

A V9-derived local baseline with the same fixes passed **11/11 CTest suites** in GCC Release, Clang Release and GCC ASan/UBSan Debug (this local copy uses the older nine-suite V7-style baseline plus V9 and V10 tests). The exact GitHub V9 branch includes **12 suites**. Confirm Linux/Windows/ASan CI at the final commit, rather than inferring it from the local test count.

## Performance caution

A small, noisy paired Linux CPU microbenchmark with a single local Grab on a long bonded chain gave roughly:
- 125k particles, previous local baseline: ~0.205 ms per **local stroke**;
- first safety patch with duplicate hash lookups: ~0.26 ms;
- optimized single-pass first-touch and reserved ID scratch: ~0.215 ms.

These numbers were obtained on a virtualized CPU in different runs and do **not** establish a robust performance delta. They exclude native Blender Undo, PBVH/Multires sample transfer and viewport. The key achievement is correct journal membership after rejected work; any speedup needs per-dab p50/p95 and peak-memory measurements on the target workstation.

## Next high-priority audit items

1. **Adaptive precision:** movement + last-substep XPBD reaction remains a heuristic. Track maximum interface reaction across all substeps and validate long multi-dab sequences on stiff connected meshes.
2. **World-scale work:** `apply_grab()` is O(N), and the adaptive BFS initializes an O(N) distance array every call; use a spatial query plus generation-mark graph scratch.
3. **Journal allocation:** each new first-touch entry is a `std::unordered_map` node. Test block/chunk snapshots and preallocated dense maps against hash-table memory and allocation cost.
4. **Contact and constitutive physics:** no volume constraints, self-collision, friction or calibrated clay model, so current solver accuracy tests are only against its own global reference.
5. **Blender integration:** no native Sculpt Mode Grab, PBVH/Undo, active Sculpt Layer writeback or editable Multires CCG extraction. Standard and 0% Assist require an exact editor-level bypass.

**Release:** keep PR #21 Draft. Stable Blender build remains unchanged.
