# Deep source audit V04 — double application of Sculpt Layers (2026-10-15)

## Scope

Branch: `fix/layer-aware-ccg-writeback-v01`. Preserve main, Conform and Bake unchanged. No heavyweight Commit inside cache teardown.

### Code-confirmed causal chain (conditional on lifecycle)

1. `source/blender/blenkernel/intern/subdiv_displacement_multires.cc`: `read_displacement_grid()` starts with native `CD_MDISPS` and adds effective `layer->disps * layer->strength` once. Different-level sampling goes via `sample_tangent()`, which also applies strength once. A second addition inside the displacement evaluator was **not found** in the inspected paths.
2. `source/blender/editors/sculpt_paint/mesh/sculpt.cc`, before commit `7341d199`: `flush_update_step()` unconditionally called `multires_mark_as_modified(..., MULTIRES_COORDS_MODIFIED)` for ALL update types. It marks image/color/mask updates as geometry edits. `UpdateType::Image` returned only *after* this marking.
3. `object_update_from_subsurf_ccg()` during `BKE_object_free_derived_caches()` calls `multiresModifier_reshapeFromCCG()` when `dirty.coords` or `dirty.hidden` remain set. The mask-only shortcut requires `!dirty.coords && !dirty.hidden`.
4. `multires_reshape_ccg.cc` copies composed CCG positions into reshape grid coordinates; `multires_reshape_util.cc` converts them into tangent-space `CD_MDISPS`. In `multires_reshape_smooth.cc`, equal top/reshape levels return early, so there is no other layer subtraction in this path.
5. On a subsequent evaluate, (1) again adds weighted layer displacement. This permits `native_new ~ native_old + weighted_layers` and `visible_new ~ native_old + 2 * weighted_layers`.

This is a proven *possible code path*; no native Blender UI reproduction/compile has been completed in this branch yet. Do not claim user-specific bug is proven until observed at runtime.

### Separate Undo hazard

`sculpt_undo.cc`: `persistent_position_swap` deliberately skips `MULTIRES_COORDS_MODIFIED` because the persistent layer payload and CCG coordinates already match. The existing source comment explicitly names Strength 0.5 apparently doubling after a subsequent flush if marked dirty again.

At lower Sculpt Levels, `DeferredPositionSwap` deliberately keeps CCG coordinates pending. Do NOT suppress these writes globally: that would break legitimate lower-level Undo/Redo. Changed layer strengths also use frame-aware paths.

### Commit and performance

`sculpt.cc` end-of-stroke calls `undo::push_end()`, which captures/finishes an Undo step, NOT unconditional layer Commit. The CCG may remain authoritative/pending until a later flush. Thus simply skipping CCG cleanup for any Sculpt Layer can lose geometry; calling `multires_flush_sculpt_updates()` from teardown is also unacceptable due to cost and lifecycle semantics.

`multires_flush_sculpt_updates()` uses `SculptChangeSet`, but `multires_sculpt_changes_sync()` re-derives position grids from PBVH changes when called. If `dirty.coords` is true but no PBVH position nodes exist, `positions_full` becomes true — causing a nonlocal expensive fallback. This makes false coordinate dirtiness especially damaging.

### Candidate fix already in branch

Commit `7341d199` correctly switches `UpdateType` domains: Position -> coords, Mask -> masks, Visibility -> hidden; Color/Image/FaceSet do not mark geometric dirt. It fixes one demonstrable mistagging and allows the existing mask-only flush. Needs native compile and A/B.

### New opt-in differential probe

`object.cc` commit `467eed27` adds read-only sampled before/after comparison around existing native reshape (no new reshape). `multires.cc` commit `146ea81b` shows explicit flush for TRACE=2. These are diagnostic, not proof or a fix. The fixed probe samples 0/middle/last grids and 0/middle/last sample positions and is **only meaningful at equal CCG/top level**.

Set `BLENDER_SCULPT_CCG_WRITEBACK_TRACE=2`. Distinguish:
- `[LAYER_CCG_WRITEBACK_V00] path=NATIVE_RESHAPE_ATTEMPT`
- `[LAYER_CCG_WRITEBACK_V00] path=EXPLICIT_FLUSH`
- `[LAYER_CCG_WRITEBACK_AUDIT] samples=... layer_signal=... projection_ratio=... residual_ratio=... valid_ratio=...`

Interpretation for a **no-position-change** test with nonzero layer samples:
- `valid_ratio=1`, `projection_ratio ~ 1`, `residual_ratio ~ 0`: sampled native displacement acquired exactly the weighted layer contribution (strong evidence for base contamination).
- `projection_ratio ~ 0` and base unchanged: no such contamination on sampled points in this call.
- `valid_ratio=0`: samples did not overlap a nonzero effective layer; INCONCLUSIVE.
- For an actual sculpt stroke or different level, the ratios cannot by themselves establish double application: real edits, smooth/rebase, tangents and procedural displacement may contribute.
- Probe coverage is intentionally sparse and cannot prove absence globally; use full-grid hashes/checksums in the final regression fixture.

### Minimum scenario matrix

1. Small, disposable Blender file: one visible layer at strength 0.5, nonzero delta over sampled first/middle/last grid locations; Sculpt = top = master, no other modifiers.
2. Baseline pre-fix (commit before `7341d199`) vs branch candidate on the SAME fixture. Color stroke, image paint, Mask stroke, cache rebuild/mode change. Trace=2. Record MDisps and layer checksum before/after; no genuine position edits.
3. Actual Position stroke, explicit Commit, repeat Commit with no new changes, mode change, Save/Reopen.
4. Undo/Redo `PERSISTENT_SWAP` top level vs `LOWER_NATIVE_PENDING` lower levels; Strength 1.0 -> 0.5 -> 1.0 independently; two layers.
5. Hidden-only dirt and mixed coords/hidden/masks; verify that no image-mapping or ownership path silently discards edits.
6. Repeated refresh 100x and memory/latency profiling at 43M/60M after all small tests PASS.

### Acceptance criterion

No geometry-neutral operation may alter native `CD_MDISPS` or any `layer.disps`. Genuine coordinate edits must persist exactly once with correct Undo/Redo. No cache teardown may call the expensive layer-aware Commit, and no unknown dirty CCG may be silently discarded. Default behavior for no-layer native Multires must remain unchanged. No Conform/Bake edits.

### Further architecture proposal (not implemented)

Introduce an explicit runtime provenance/ownership state distinguishing `DerivedClean`, `PendingPositionEdits`, and `PersistedPositionEdits`. Only the authorized Sculpt edit owner may publish pending coordinates. Cache cleanup never guesses based on `dirty.coords`. A transition that would destroy pending edits must be committed at a safe operator/stroke boundary or preserve the authoritative CCG until publication. Account for depsgraph invalidations that may occur mid-stroke. Reuse the existing dirty-grid and exact-sample journal; avoid DNA edits if possible. Lower-level and procedural paths retain conservative fallback.

No automatic commit is scheduled on cache destruction.
