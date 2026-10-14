# Layer-Aware CCG Writeback V01 — isolated investigation

Base: branch `feature/sculpt-polypaint-undo-20261014`, checkpoint `cea35a4f31f`. This branch is isolated; do not merge into `main` or apply any patch to `C:\BlenderDev\blender` by default.

## Observed code paths (not yet proof of the reported bug)

- `BKE_object_free_derived_caches()` calls `object_update_from_subsurf_ccg()` (`source/blender/blenkernel/intern/object.cc`).
- When evaluated CCG dirty flags are set, the latter calls `multiresModifier_reshapeFromCCG(tot_level, mesh_orig, subdiv_ccg)` and publishes `CD_MDISPS` to original and CoW mesh without checking whether the evaluated CCG already contains Sculpt Layers.
- The Memfile Undo guard blocks that writeback only when its optional scoped flag is active.
- The explicit `multires_flush_sculpt_updates()` (`source/blender/blenkernel/intern/multires.cc`) has a layer-aware solver, preserving native base and updating the selected layer through local or scratch-reshape routes.

These are two distinct writeback paths. The unsafe native path is a strong *hypothesis* for double application, not a confirmed trigger.

## Surface invariant

For an additive effective layer stack and a fixed tangent/cage frame:
`D_visible = D_native + sum(strength_i * D_layer_i) + D_procedural`.
An operation not intended to change sculpt geometry must not modify persistent native displacement or layer payloads; the visible surface must stay unchanged. Real sculpt edits must be persisted **exactly once** by the owner of the active transaction.

Conform and Bake are explicitly out of scope.

## V00 — read-only tracing, no behavior changes

Instrument the evaluated-cache teardown site and explicit flush, gated behind one environment flag:
- reason / operation, original vs evaluated object pointers, evaluated CCG address;
- exact match with current `SculptSession::subdiv_ccg` if present; active modifier / level / master level;
- dirty.coords/hidden/masks, active layer, recording state, visible/effective layers;
- before/after stable sample checksums of native MDisps, active and non-active layer displacements;
- before/after visible CCG positions and presence of change journal.

Do not rely on dirty.coords alone to distinguish genuine strokes from derived data. Do not globally skip cleanup or clear dirty flags.

## V01 — policy after trigger is reproduced

1. For an original/evaluated CCG that is proven to be the *same* live sculpt transaction, route actual pending edits through existing `multires_flush_sculpt_updates()`, before changing levels/strength; preserve errors rather than falling back to native reshape.
2. For old/derived layer-composed CCG with no owned edit transaction, do not persist its coordinates as native base. But never discard genuine pending edits merely because layer data exists. Verify ownership and lifecycle before installing a skip branch.
3. For no effective layers, preserve existing native path. Masks-only already have a distinct path. Treat hidden changes and mixed dirty flags separately.
4. Avoid direct subtraction of layer deltas from post-reshape MDisps for lower-level reshapes: remapping/tangent frame and procedural displacement make this nontrivial.
5. No DNA changes, no Conform/Bake changes, no numerical strength hacks.

## Required reproduction matrix

- One visible layer strength 1.0 and 0.35, two visible layers, plus muted/all-hidden.
- No-op refresh (repeated 10/100x), actual brush stroke and explicit Commit, strength 1 -> 0 -> 1, mode changes, save/reopen.
- Subdivision at master level and lower sculpt level; Undo/Redo without forced save between stroke and undo.
- Procedural displacement on/off; masks-only and mixed dirty flags.
- Evaluate exact persisted base, each layer delta, evaluated surface, grid IDs, and writeback count before/after. A no-op must not increase amplitudes; an edit must survive cleanup exactly once.

## Acceptance gates

- Detect and record the exact first writeback that changes a native grid unexpectedly.
- Baseline A/B on the same fixture and deterministic sequence.
- V01 must preserve genuine pending strokes and their Undo/Redo; never mark dirty false unless publication succeeds.
- Native builds and UI regression tests on Windows before creating portable or merging.
