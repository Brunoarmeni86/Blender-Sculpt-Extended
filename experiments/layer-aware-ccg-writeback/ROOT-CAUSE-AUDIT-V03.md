# Double Sculpt Layer Application — source audit V03 (2026-10-15)

Base: isolated `fix/layer-aware-ccg-writeback-v01`. Main and prior development checkpoint untouched.

## Code-confirmed problem 1: wrong dirty domain for paint/mask redraw

**File:** `source/blender/editors/sculpt_paint/mesh/sculpt.cc`, `flush_update_step(ViewContext&, Object&, UpdateType)`.

Before V03, **every** Sculpt stroke update having `ss.multires_modifier != nullptr` called:
```cpp
multires_mark_as_modified(vc.depsgraph, &object, MULTIRES_COORDS_MODIFIED);
```
This includes `UpdateType::Mask`, `UpdateType::Color` and `UpdateType::Image` (see `SculptPaintStroke::update_step`). It can mark `SubdivCCG::dirty.coords` with no position modification. Image's early return ran **after** the false coordinate dirtiness was set.

**File:** `source/blender/blenkernel/intern/object.cc`, `object_update_from_subsurf_ccg`. Its cheap mask-only flush is gated on `dirty.masks && !dirty.coords && !dirty.hidden`. False coordinate dirt forces the general `multiresModifier_reshapeFromCCG()` path. That routine stores CCG-derived positions into native `CD_MDISPS`. If CCG contains weighted Sculpt Layers and the independent layer payload remains active, this *can* embed layer deformation into the base and reproduce a doubling after reevaluation.

### Applied code correction
Commit `7341d19903ab3d0ce9e7a880ece29c127be094f0` maps:
- Position -> MULTIRES_COORDS_MODIFIED
- Mask -> MULTIRES_MASK_MODIFIED
- Visibility -> MULTIRES_HIDDEN_MODIFIED
- Color/Image/FaceSet -> do not mark Multires geometric dirt.

This is a small, functional candidate fix, no Commit and no DNA changes. **It is not native-compiled or Blender-tested yet.**

### Exact behavior to verify
A Mask brush must produce `dirty.masks=1, dirty.coords=0` and take `multires_flush_mask_updates` without touching native `CD_MDISPS`. Image/Color must not set dirty.coords. A Position stroke must still set dirty.coords and persist real changes.

## Code-confirmed problem 2: native evaluated-cache writeback is not layer-aware

`object_update_from_subsurf_ccg` calls `multiresModifier_reshapeFromCCG(tot_level, mesh_orig, subdiv_ccg)` when dirty flags survive to evaluated-cache teardown. No effective-layer check is made. This is distinct from the layer-aware, potentially expensive `multires_flush_sculpt_updates` path.

Do not fix by blindly skipping the native call, because real pending sculpt coordinates might otherwise be destroyed when the evaluated cache is freed. Do not invoke expensive layer-aware Commit from teardown (V01 local patch explicitly withdrawn).

## Code-confirmed problem 3: persistent Position Undo already protects one double-publish case

`source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc`, `restore_list`, skips `multires_mark_as_modified(... MULTIRES_COORDS_MODIFIED)` when `persistent_position_swap` holds. The existing comment explicitly mentions strength 0.5 appearing to double if already restored persistent layer payload and CCG coordinates are committed again.

Other paths deliberately use pending CCG positions, including lower-level native/deferred swaps. Therefore a blanket “Undo coordinate change is clean” rule is incorrect.

## Unresolved paths

1. Real Position stroke pending while depsgraph cleanup runs (no inexpensive complete solution currently proven).
2. `dirty.hidden` alone still reaches general native reshape; the mask-only fast-path does not cover it. Evaluate a separate visibility-only flush without touching coordinates.
3. Mixed dirty flags (positions+mask, positions+hidden) need exact ownership.
4. Lower Sculpt Levels and active procedural displacement need frame-aware conversion, not naive subtraction.

## Required native test matrix

- Baseline with one visible layer at strength 0.5 and 1.0; mesh with nonzero displacement; capture base `CD_MDISPS` and per-layer grid arrays independently.
- Color stroke -> force evaluated-cache teardown -> base stays identical, layer stays identical.
- Mask stroke -> force teardown -> masks change, base/layers unchanged.
- Image Paint -> force teardown -> base/layers unchanged.
- Actual Position stroke -> explicit Commit -> one layer update, base untouched. Repeat Commit (no-op).
- Pending Position stroke -> mode change and save; no edit may be silently dropped.
- Undo/Redo: persistent top-level swap, lower-level deferred swap, changed layer strength/visibility.
- Hidden-only and mixed dirty: inspect path, do not approve until persistence is correct.
- At each step log `[LAYER_CCG_WRITEBACK_V00]`; record base/active-layer payload and visible geometry. Test *before* and *after* to identify the first offending write.
- Benchmark Commit and redraw on representative dense mesh after correctness passes.

## Proposed subsequent fix direction

Introduce an explicit owner/provenance guard for pending **position** edits. The evaluated-cache teardown may drop only *proven derived-only* CCG state; a genuinely pending stroke must have been published at a safe transaction boundary, or teardown must refuse a destructive transition. Avoid unconditional heavy commits and unnecessary full-surface scans. Prefer the existing sparse `SculptChangeSet` and exact position journal for genuine edits, but note journals can fall back to full updates and are not authoritative for all writers.

No Conform or Bake edits in this workstream.
