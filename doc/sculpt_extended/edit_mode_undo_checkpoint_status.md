# Edit Mode / Sculpt Layers Undo: intermediate checkpoint, 2026-10-12

## Decision

Keep the exact production source at `3db7a1077faa7c9257d141d668273743d8bb2bf6`
in `checkpoint/edit-mode-no-corruption-20261012`. Do not merge into main.

The user reports that this candidate no longer produces the previous corruption,
but Undo loses the prior sculpt detail and returns to the base before the stroke
and Edit operation. This is an intermediate checkpoint with a known limitation.

The subsequent candidate `af4582a2f9109b8061394a846f4845f02cef2907`
(`fix/undo-mode-boundary-checkpoint`) was tested by the user and rejected with
"NIENTE": it did not fix the reported behavior. Its isolated mocked helper tests
did not predict successful native restore and must not be cited as runtime proof.

Main remains `83d81b2dfc2af8ba7763d33fce212dbaa9b5dc49`.
Conform UI and CPU Bake changes are separate. No experimental GPU baking is included.

## What the latest source review establishes

- `editmesh_undo.cc`: MeshUndoStep_Elem holds an object reference and UndoMesh.
  UndoMesh stores a Mesh, BMesh selection/shape-key data, and custom-data array
  storage; there is no Multires modifier layer snapshot in that element.
- `MOD_multires.cc`: Sculpt Layer delta payloads are serialized with the modifier,
  attached to the Object, rather than as BMesh corner custom data.
- `EDBM_mesh_load_ex`: after BMesh-to-Mesh conversion it remaps current modifier
  payloads using the previous mesh corner IDs and the newly restored/edited IDs.
- `remap_surviving_corners`: surviving layer grids transfer ownership. Unmatched
  new grids are zero, and untransferred previous payloads are freed. Returning
  an old ID through Edit Undo alone cannot resurrect a freed payload.
- `undo_system.cc`: native ID-reference decoding may restore the preceding
  memfile before decoding a mesh/sculpt step. `memfile_undo.cc` also updates
  native is_applied flags. These interactions remain part of the investigation.
- Position stroke undo stores compressed grid coordinates and a layer composition
  frame. Its grid topology guard is separate from the Edit mesh undo state.

These are code findings. The absence of a per-Edit-step layer snapshot is a
concrete gap, but this review has NOT demonstrated which native decode/flush
sequence causes the user's full return to the pre-stroke base. No post-failure
stack trace, payload checksums, or native Blender reproduction is available here.
Do not claim that stored data is absent, correctly saved, or overwritten in this
specific run without measuring it.

## Why stop incremental guesses here

The four mode-operator checkpoint hooks addressed a real exclusion in the source,
but the native test failed. Adding further flushes, rebuilds or global snapshots
without observing the actual decode order risks more memory cost and loss of
history while failing to restore the missing detail.

The remaining robust design to investigate is a topology undo state that owns
both mesh identity/layout and the corresponding layer payload state, with an
explicit handoff to native stroke undo. It must restore both coherently before
recomposing CCG grids. This is a design direction, not an implemented fix.

A future implementation needs to retain deleted-grid payloads for Undo without
making a dense copy of every layer on every stroke or Edit selection action.
Shared immutable sparse snapshots or deduplicated storage should be evaluated,
including L0, masks, polypaint and face-set data. Visibility and strength changes
must keep their own property history.

## Resume gate

Before another Windows patch, reproduce the exact mode/action sequence in native
Blender and record active undo type/name/is_applied, corner IDs and grid layout,
and checksums of persistent deltas versus CCG before Edit and at each decode.
Use targeted instrumentation that does not alter write/restore behavior.
Compile and test the final candidate with manual-Commit controls and repeated
Undo/Redo, Delete/Fill with unchanged corner counts, and large sparse scenes.

## Revert the failed boundary candidate only

The distributed `undo-mode-boundary.patch` changes only ED_undo.hh, ed_undo.cc,
and wm_event_system.cc. Reverse that exact patch with git apply -R --check,
then git apply -R, and rebuild the existing Release INSTALL target. This keeps
the stable-corner-ID implementation and independently applied Conform changes.
Do not reset the entire Windows source directory or move main.
