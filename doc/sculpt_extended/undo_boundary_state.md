# Undo: flush old composition before Sculpt-entry boundary correction

Parent: 4031e4d3c2c10d99c28f5aba20a6bc94d0b7070b.
The user's recording includes Delete + Fill interpolation. Those folded/pinched
shapes were explicitly identified as the expected topology-edit result, not
proof of Undo corruption. The unwanted symptom is restoration of an empty
pre-stroke state when the user expects previous detail to remain. Which exact
Undo step causes this is not yet established by a native state trace.

## Concrete ordering correction

sculpt_layer_restore_position_boundary_frame previously changed sculptlvl,
active layer and weights BEFORE multires_force_sculpt_rebuild, and ignored
that function's failure. Its old comment assumed persistent Undo had already
synchronized deltas. Coordinate-authoritative Undo can leave CCG data pending.

The normal Multires flush validates CCG level against current sculptlvl and
solves layer deltas with CURRENT composition weights. Switching those first
can reject the old CCG or interpret coordinates in the wrong composition.

Candidate: flush/release the CCG in the OLD modifier frame, aborting boundary
correction on failure while retaining pending data. Only then set captured
metadata and reevaluate. No additional second flush/rebuild after changing
metadata. If no metadata correction is needed, no new flush is performed.
This fixes a real ordering defect, but is not evidence that the user's empty
state is caused by this particular branch. Native runtime validation pending.

## Optional state tracing

BLENDER_SCULPT_LAYER_UNDO_TRACE=1 prints:
- DECODE_ENTER / DECODE_READY: before/after ensuring Sculpt Mode.
- UNDO_BEFORE / UNDO_AFTER or REDO_BEFORE / REDO_AFTER.
- BOUNDARY_BEFORE / BOUNDARY_AFTER around Sculpt-entry frame correction.

UNDO_STATE includes step name/type, mode, mesh counts, recorded/live grid
layout and coordinate/hidden/mask dirty flags. UNDO_LAYER_STATE includes level,
visibility, strength, grid count and a bounded sampled energy/signature.
Trace is read-only, disabled by default, and samples at most 256 grids x three
points per layer. A zero sampled energy does NOT prove the whole layer empty.
It does not trace mesh-edit/memfile decode callbacks themselves, but captures
the state they leave before Sculpt decode. It does not allocate mesh copies.

## Verification

Extracted boundary code compiles with minimal types and checks old-level flush,
old-weight solve, failure retention and metadata order. Actual tracing function
compiles with minimal data types; it prints only when enabled and leaves state
unchanged. CCG-policy guards/960-exchange scalar simulation and legacy
consistency guards/320-exchange scalar simulation still pass.

No native Blender build/runtime tests, BMesh simulation or CCG reshape geometry
validation. Main, Fill interpolation, Conform UI and baking are unchanged.

## Runtime test

Use a small copy of the same scene, enable trace, and repeat the exact video
sequence once, without extra Undo presses. Save console log. Check which
step name is restored when detail disappears and whether it happens before
Sculpt restoration, during coordinate exchange, or at boundary correction.
Annotate which Undo should cancel Fill/Delete versus the original stroke.
If only the original stroke is being undone, an empty pre-stroke surface may
be the correct state; do not preserve a stroke that the user explicitly undid.
