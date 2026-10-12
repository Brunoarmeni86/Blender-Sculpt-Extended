# Stable corner identities for Multires layer grid remapping

Parent reference: 4031e4d3c2c10d99c28f5aba20a6bc94d0b7070b.
This patch does not modify sculpt_undo.cc, UndoStack routing, native interpolation,
Conform, baking or the experimental checkpoint-control policy.

## Evidence and mechanism

The user's screenshot sequence 40-44 shows valid layer detail before Edit Mode,
a small local Delete/Fill result, and a much larger deformation after Undo.
The earlier mapping used ID-1 as the modifier array index and reset all corner
IDs to index+1 after remapping. Edit undo snapshots can hold IDs from an older
layout. Same corner counts can then satisfy the identity fast path while the
modifier payload belongs to a different order.

The minimal model has 24 grids (six quad faces). Delete the second face and
append a Fill face: surviving deltas are compacted and four zero grids appended.
Resetting IDs lets the restored original 24 IDs pass the old identity check;
20 grid payloads differ from the original association. This reproduces an
indexing error without simulating interpolation or brush geometry.

## Candidate implementation

- Capture the old mesh corner IDs BEFORE BM_mesh_bm_to_me replaces attributes.
  That array describes the current modifier grid order.
- Lookup candidate IDs in the captured old-ID-to-array-position map. Never use
  numeric ID-1 as a persistent grid position when the supplied old map exists.
- Retain surviving IDs. New/ambiguous/orientation-changed corners get fresh IDs.
- Preserve the existing duplicate, cyclic-neighbor and face-size checks.
- Publish previous/next IDs from stable identities rather than array indices.
- On save/render flush while staying in Edit Mode, copy the published stable
  IDs back to BMesh so repeated flushes do not replay stale indexing.
- Keep a monotonic high-water mark in a mesh system ID property. It survives
  BMesh replacement and geometry deletion without adding DNA fields or a
  point-domain attribute. Overflow and malformed counter data reject remap.
- Move existing displacement ownership as before. No high-resolution layer
  payload duplication, reprojection, or global rebuild is introduced.

The empty optional old-ID span remains a legacy canonical mapping fallback.
The edited EDBM caller supplies the real old order. Any future caller working
with stable noncanonical IDs must do likewise.

## Explicit limits

This fixes association of SURVIVING payloads. It cannot recover a deleted
layer grid whose payload was already freed by an earlier topology remap. Such
corners remain zero-delta unless a matched real undo checkpoint restores their
payload. Do not claim complete layer-data recovery across Delete Undo.

Vertex IDs/L0 base-delta remapping retain their previous implementation and
need a separate audit for equivalent historical-index problems. This candidate
focuses on high-resolution corner/grid data. Ordinary new topology still starts
with zero layer delta. Native MDisps interpolation is unchanged.

## Verification

The actual portable corner-lookup header and extracted ID-property counter
helpers compile with minimal adapter types. Cases cover noncanonical IDs,
permutations, duplicated candidates, invalid/duplicate source IDs, empty layouts,
maximum IDs, same-count Delete/Fill restore, counter persistence/monotonicity and
1000 shuffled layouts. ASan/UBSan pass with leak checking disabled because this
executor's process instrumentation prevents LeakSanitizer reading /proc tasks.

Blender API signatures for mesh system ID properties and VArraySpan inheritance
were checked against the actual repository headers. Native Blender compilation,
BMesh conversion, .blend serialization and runtime Undo tests remain pending.

## Windows test

Close/reopen Blender with CCG Undo on, checkpoint-control experiment off and
native trace off. Recreate the screenshot sequence on a small copy. Check that
Undo does not spread the local Fill deformation onto unrelated grid regions.
Also test repeated Edit save/exit, different layer strengths, multiple layers,
and save/reload. Inspect any loss of detail in the deleted area separately.
The patch can be reverse-applied independently of the sculpt_undo candidates.
