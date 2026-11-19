# V8 — Deferred Undo, XPBD multiplier safety and redundant fallback checkpoints

**18 November 2026 — Experimental standalone CPU C++17, PR #21 Draft. Not a Blender build.**

## Findings: two reproduced bugs

### 1. Rejected global dab permanently promoted the stroke journal to dense

Previous code called `upgrade_to_dense_journal()` at the **beginning** of `World::step()`. Even if the global solve subsequently threw and correctly rolled back particle/bond state, a previously empty or sparse stroke Undo was already converted into a full-world snapshot.

**Reproducer:** one particle, begin stroke, valid-but-extreme finite Grab, global step throws from a non-finite velocity. Before fix, `stroke_journal_statistics().dense_fallback == true` despite the failed dab; after fix, it remains `false` with zero particle entries.

**Fix:** the existing **current-step rollback vectors** become the dense **pre-stroke** Undo snapshot **only after the global step completes successfully**. Before moving them to Undo, overlay original first-touch particle/bond values recorded by earlier successful local dabs. On failure, leave the sparse stroke journal unchanged while restoring the current step state. This also eliminates a second O(N+M) full-state copy during the first successful global step.

Regression covers a successful local plastic dab, failed global dab, later successful global dab and exact whole-stroke cancel.

### 2. Non-finite XPBD material multiplier without an exception

Even if `h*h` is positive, it can be subnormal, and `compliance / (h*h)` can overflow to infinity. A pinned-pinned bond with normal-looking finite coordinates could produce `0 * infinity / infinity = NaN` in XPBD multiplier updates, return success, and leave `bond.lambda = NaN`.

**Reproducer:** two pinned particles at x=0 and x=1, compliance 1.0, `dt=1e-160`. Previous source returned success with nonfinite bond multiplier. The V7 check only rejected `h*h == 0`, not positive subnormal values.

**Fix:** reject subnormal squared substeps; additionally validate computed `alpha`, correction multiplier and its accumulated value for all distance bonds and Grab target constraints. Non-finite bond length during a step now also raises an exception. A failed step restores the old state via V7's checkpoint.

The new regression also covers large finite compliance (`1e100`) with normal `h*h=1e-300`, which yields overflowing scaled compliance even though the substep itself passes the normal-range check.

## Fallback checkpoint optimization

Previously `step_local_impl()` and the adaptive solver's `run_global` copied the entire world **before calling `World::step()`**, which already creates its own full per-dab rollback checkpoint. The duplicates were removed. The global call now owns atomic rollback and Undo conversion; the caller only marks `used_global_fallback=true` after success.

No change to the mathematical constraints or the opt-in multi-island policy is intended.

## Tests

Standalone local builds from a copy of the V7 source:
- GCC 14.2 C++17 Release: **10/10 CTest suites passed**.
- Clang Release: **10/10 passed**.
- GCC Debug AddressSanitizer + UndefinedBehaviorSanitizer: **10/10 passed**.

New suite: `test_v8_transactions.cc` (failed global sparse Undo, local→failed global→successful global→cancel, tiny timestep, overflowing material compliance, pinned constraints). The exact final GitHub Windows/Ubuntu CI result must be checked separately.

## Preliminary CPU timings

Paired V7/V8 tests on the same virtualized Linux CPU, GCC 14.2, C++17 optimized; reported figures are medians after one warmup. The benchmark is **standalone solver-only**, not full brush latency; no PBVH, viewport, high-res Multires writeback or Blender Undo.

**Sequential distant-stroke fallback** with sparse unbonded background (2 dynamically active particles, rest stationary):

| Total particles | V7 double-checkpoint fallback | V8 single-checkpoint fallback |
| ---: | ---: | ---: |
| 10,000 | 0.858 ms | 0.452 ms |
| 50,000 | 4.544 ms | 2.766 ms |
| 125,000 | 12.377 ms | 6.555 ms |

Another bundled `bench_stroke_undo` test, 50k-particle bonded chain, first global stroke, measured ~5.35ms V7 vs ~5.15ms V8. This is a different workload and not a statistically robust global solver speedup claim. Hot-loop validity checks may offset savings in heavily constrained connected meshes.

**Interpretation:** removing redundant O(N+M) copying helps fallback-heavy sparse interactions. It does not accelerate XPBD convergence or reduce error in the local boundary approximation.

## Remaining issues and guards

1. **No real Blender integration:** native Sculpt PBVH/Undo and Multires active layer dirty-tile writes are unimplemented; Standard and 0% Assist paths must retain a true editor-level no-op.
2. **Full-world per-dab checkpoint:** still necessary for atomic failures under the present design; consider block-level transactional deltas or a scratch simulation buffer, but benchmark real allocation and cache costs.
3. **Numerical conditioning:** huge inverse mass/radius/dt and NaN propagation via other material parameters still require differential and fuzz tests. Rigid/incompressible models need calibrated constraints and convergence metrics.
4. **Adaptive boundary tolerance:** currently heuristic, not a certified bound; multi-dab plastic/fracture tests are required.
5. **Scratch ownership and reentrancy:** `World` has mutable buffers; not safe for concurrent sculpt calls.
6. **Configuration cancellation and redrawing:** gravity/damping/ground changes during a stroke and native PBVH invalidation on cancel remain unhandled.
7. **Material realism:** distance bonds alone lack volume preservation, particle contact/friction and stable fracture surfaces.

**Release status:** leave PR #21 Draft and the stable Blender release unmodified.
