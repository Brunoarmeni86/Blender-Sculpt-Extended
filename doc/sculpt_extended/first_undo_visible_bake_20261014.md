# First Undo after layer selection + visible-only base bake (2026-10-14)

Experimental Windows validation branch, based on stable main `83d81b2d`. Do not merge into main before native Blender testing.

## 1. Bake Visible Layers into Base

`source/blender/blenkernel/intern/multires_sculpt_layers.cc`:
- Incorporate *only effective visible* layer deltas, including negative Strength, into `CD_MDISPS`.
- Preserve hidden layer payloads exactly (including hidden L0 data), and preserve visible zero-strength payloads.
- Clear baked visible displacement buffers only **after** a full finite-values validation pass.
- Reject non-zero visible L0 base-cage deltas: their tangent-space transfer requires a distinct reshape operation.
- Leave cage positions and existing Multires levels unchanged; require single-user mesh and compatible top-level grids.
- Force cache invalidation after a successful bake.

## 2. First Undo after selecting an existing layer

`source/blender/editors/sculpt_paint/mesh/sculpt_ops.cc`:
- `Set Active Sculpt Layer` commits old pending strokes first.
- Treat selection as metadata-only, without forced CCG/PBVH rebuild and without a separate global Undo step.
- Use `ID_RECALC_SYNC_TO_EVAL` and a modifier notifier.
- Test that the first Ctrl+Z undoes the last stroke, not the layer selection or a stale base-mesh snapshot.

## Required native Blender regression tests

1. On a COPY of the 7-layer reproduction file, use Sculpt Level 2 / Total Level 5.
2. Select a previously existing layer using the UI. Make a stroke. Undo once, Redo once. Check both the surface and each layer's persistent payload; native base must not change. Repeat with Strength 0.5, -0.5, and layer visibility changes.
3. Repeat selection + Undo *without* painting: it must not create a geometry/global Undo step for selection alone.
4. Bake with two visible layers (e.g. Strength 1 and -0.5), one hidden layer and one visible zero-strength layer. Compare visible surface before/after across levels 2 and 5; hidden payload remains unchanged, selected deltas are cleared, and toggling hidden afterwards still adds its contribution.
5. Undo and Redo the bake; check both native `CD_MDISPS` and layer buffers, then save/reopen the .blend and repeat.
6. With a visible non-zero L0 cage edit, bake must fail without mutating the mesh or any layer data.
7. Check RAM/CPU on high-resolution scenes separately after correctness passes.

## Status

- Code inspection: complete.
- Pure C++ isolated transfer test: PASS on Linux, testing visible and negative weights, hidden and zero-strength preservation, rejection of edited L0 and NaN.
- Blender Windows compilation: **not performed**.
- Integration Undo / Redo / save-reload: **not yet verified**.
- This branch is an initial candidate, not a definitive resolution of the first-Undo problem.
