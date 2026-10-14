# Layer-Aware CCG Writeback — V02 design: no expensive commit during teardown

Status: DESIGN / DIAGNOSTIC. No production fix approved.

## Supersedes V01 policy

**Do not enable `BLENDER_SCULPT_CCG_WRITEBACK_FIX=1` from the V01 local patch.**
The V01 experimental implementation called `multires_flush_sculpt_updates(object_orig)` from `object_update_from_subsurf_ccg()` during evaluated-cache teardown. This can perform expensive local or global solves, and teardown can occur for reasons unrelated to an actual stroke. Its `REFUSE_NATIVE` branch also had a risk of losing pending edits when the evaluated cache is freed. Therefore V01 is **not** approved as either a performant or safe fix.

The V00 instrumentation already committed to this GitHub branch is unaffected and safe as a read-only diagnostic. The separate V01 patch was distributed as a file; it was not applied to the GitHub branch.

## Target invariant and ownership

- Evaluated CCG contains base + visible weighted layers (+ procedural displacement if configured).
- Only real sculpt edit transactions have authority to publish new persistent data.
- The evaluated cache is **not** the owner of persistent Sculpt Layer payload.
- A cache teardown cannot call a heavyweight layer-aware Commit.
- No deallocation may silently discard uncommitted changes; install reliable pre-teardown flush at proper sculpt transaction boundaries before changing cleanup semantics.

## Proposed implementation stages

### V02a — inexpensive provenance, no behavior change

- Inspect the producer of `dirty.coords` and the `SculptChangeSet` journal; dirty alone does not prove a stroke.
- Record at trace level the original/evaluated CCG pointers, Sculpt Session owner, dirty flags, journal revision and touched grids, the current modifier, and whether this invocation follows explicit commit.
- Measure call frequency and time cost of explicit local/global Commit and of native reshape, without triggering either from tracing.
- Reproduction must identify the first surprising native CD_MDISPS write. Compare before/after base displacement and each layer payload, not solely visible shape.

### V02b — explicit edit publication at safe boundaries

- Publish actual pending sculpt deltas **once** at existing stroke/undo/mode-transition owners using the existing local Commit (exact dirty samples and cached frames) whenever applicable.
- If no real edit is pending, don't run Commit.
- For lower sculpt levels, live procedural displacement or unsupported changes, retain the verified fallback solver on the explicit edit path.
- Ensure pending edits cannot reach evaluated-cache destruction without being committed or safely retained; do not use a void teardown method as the place to make a fallible flush decision.

### V02c — cleanup policy

- Cache teardown with proven derived-only CCG and no pending edit journal: discard evaluated coordinates without writeback into original `CD_MDISPS`.
- Cleanup with pending or uncertain edits: assertion/diagnostic and preservation via prior safe boundary; **do not silently skip** on the strength of a `dirty.coords` or pointer test.
- No DNA or Conform/Bake/Strength changes.

## Reproduction and performance gates

1. Zero edits, repeated 100x evaluation/mode changes: zero heavyweight Commit calls; base and all layer payloads unchanged.
2. One actual stroke on one dirty grid: one explicit publication of just dirty samples; CCG teardown itself never performs layer Commit.
3. Strength 1.0 and 0.35, two layers, muted/hidden, Undo/Redo, save/reopen, upper and lower Sculpt Levels.
4. Native no-layer Multires and mask-only changes remain correct.
5. Compare log timings `COMMIT_LOCAL`, `LAYER_COMMIT_*`, and whole operation wall times, not summed worker CPU times.
6. Stress with ~43M and ~60M vertices, then bigger meshes after smaller fixtures pass.
7. Mark release PASS only after native MSVC build and actual Blender UI tests.

## Do not do

- Do not call Commit from `BKE_object_free_derived_caches()`.
- Do not assume `ccg->dirty.coords` means dirty layer displacement.
- Do not unconditionally `return` from cleanup with genuine pending edits.
- Do not subtract layer deltas directly from reshaped MDisps at lower levels without proper frame mapping.
- Do not reuse the production build or modify `main`.
