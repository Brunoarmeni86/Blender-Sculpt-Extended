# Sculpt Layer Undo: code audit and synchronization candidate

Reference: 84d4368c101d948811be9c9b0eef69caeaf131c4, building on the stable
83d81b2 baseline, Edit remap/interpolation, compression wait correction and
all-hidden native-base candidate. Conform UI remains on its separate branch.

## Observed user evidence

- Intermittent layer corruption around Edit -> Sculpt -> Undo without Commit.
- Manual Commit before Undo avoids the observed defect.
- Removing layer data removes the visible corruption.
- LOWER_NATIVE=0 did not remove it.
- PERSISTENT_V3=0 stopped the reported corruption; normal Sculpt strength and
  Undo/Redo seemed correct and fast at about 6M polygons in a new scene.

These observations implicate persistent layer restore, but do not prove a
single cause or establish correctness of all topology/mode boundaries.

## Inspected ownership and execution order

sculpt.cc stroke_undo_begin calls undo::push_begin_ex. save_common_data records
composition and level before node payload capture. fill_node_data_grids saves
CCG coordinates and separately copies the active layer's stored top-level
MDisps. No pre-capture Commit is performed. Later stroke position updates mark
Multires coordinates dirty. Layer deltas may therefore lag behind the live CCG.

multires_flush_sculpt_updates checks pending CCG flags, resolves the session's
modifier, and converts live coordinates back into the active layer. At top
level its local path can update touched regions; lower-level/fallback reshape
can affect the entire stored high-resolution surface. On success it clears
CCG dirty flags and the PBVH change journal.

sculpt_layer_undo_persistent_restore commits live coordinates before exchanging
saved deltas. In an unchanged top-level frame it then exchanges native CCG
positions and leaves them clean, assuming both saved representations agree.
In a changed composition/level it refreshes CCG from persistent data, returning
without exchanging compressed_positions.

Visibility and Sculpt Level RNA setters commit the OLD frame/level before
changing it. Sculpt exit also flushes before freeing the session. These
boundaries can expose persistent/CCG disagreement that was invisible until
re-evaluation.

## Defect 1: stale persistent capture paired with current coordinates

Example at strength 1 (one displacement component):

| State | Stored delta | Live CCG offset | Dirty |
| --- | ---: | ---: | --- |
| Initial | 0 | 0 | false |
| Stroke A, no Commit | 0 | 1 | true |
| Before stroke B capture | 0 | 1 | true |
| Stroke B | 0 | 4 | true |
| Pre-fix Undo B | 0 | 1 | false |

Undo B commits 4, swaps the captured stale delta 0 back, and separately restores
captured CCG position 1. It marks this inconsistent pair synchronized. A later
refresh exposes the wrong persistent surface. This defect follows directly
from the two storage paths; no Edit Mode operation is needed in the simulation.

Candidate: decide persistent snapshot eligibility once, before the stroke,
requiring clean CCG coordinates/hidden/masks and matching maximum stored/live
levels. Pass that decision into both node-capture entry points, including the
parallel path; worker threads do not consult the UndoStack. Pending/lower-level
strokes retain native coordinate undo plus captured composition metadata.
Same-frame native restore leaves coordinates pending for the ordinary flush.

## Defect 2: persistent refresh breaks coordinate-buffer pairing

Example: capture a stroke at strength 1, change strength to 0.5, Undo, change
back to 1, Redo. The first Undo uses persistent refresh and does NOT exchange
compressed_positions. The subsequent Redo previously took same-frame native
position exchange anyway. It could restore the after-stroke delta while still
showing before-stroke coordinates, marking the mismatch clean again.

Candidate: each step tracks whether coordinate buffers still pair with the
persistent undo/redo buffers. After a successful persistent refresh, mark the
coordinate pair invalid. Future restores for that step recompose from its
restored persistent data instead of using stale compressed positions. Normal
same-frame top-level steps retain the original cheap exchange.

## Frame fallback and lower-level scope

If persistent restore declines, enable the existing recorded-frame coordinate
path while PERSISTENT_V3 is enabled. Compare actual active layer, strengths,
visibility and level; equal frames use native coordinate exchange. Different
frames use the existing flush/recompose/restore-frame machinery. Keep existing
L0, stack, modifier, topology-count and session guards. PERSISTENT_V3=0 retains
its diagnostic semantics, including the all-hidden native-base support.

Do not record partial top-level persistent snapshots for lower-level strokes:
a detail-preserving lower-level Commit may change grids outside the touched
PBVH nodes. Such snapshots cannot represent that complete transaction.

## Other audit findings, not claimed fixed

- The previous compression patch waits before reading asynchronously published
  grid buffers. Both waits remain present in this candidate.
- Sculpt topology_matches validates counts/grid size, not connectivity or grid
  orientation. Same-count topology edits need a separate identity audit.
- Edit mesh undo restores BMesh custom data; it does not itself snapshot the
  modifier's Sculpt Layer stack. Crossing memfile/edit/sculpt boundaries still
  needs integration validation, especially topology removal and remapping.
- Persistent payload exchange validates descriptors and lengths before mutation,
  but late materialization/compression failures are not a fully staged rollback
  transaction. No allocator failure behavior is changed here.
- Native all-hidden support and frame rebasing retain pre-existing L0-frame
  safeguards. Their runtime and cross-level correctness are not established
  by the algebraic simulation.

## Verification and limits

The standalone test compiles the actual snapshot-eligibility and refresh-route
C++ expressions with minimal types. It covers 32 visibility/dirty/level flag
combinations, a CCG-level mismatch, and coordinate pairing/refresh decisions.

An exact rational scalar model reproduces both pre-fix inconsistencies and
passes after the corrected routing. It covers 32 scenarios across positive,
negative, fractional and zero display strengths, pending vs committed capture,
and 160 Undo/Redo round trips (320 exchanges).

This is a synchronization model, NOT Blender geometry simulation. It does not
execute multires reshape, tangent-space conversion, PBVH, BMesh, compression,
memfile restore, or the native UndoStack. No Blender executable or full source
checkout is available locally. Native compilation and real runtime tests are
pending. Do not call this candidate a complete Undo fix or merge it into main.

## Windows validation

Re-enable PERSISTENT_V3 for the candidate test. Use a copy of the test scene.

1. At max level: consecutive overlapping/non-overlapping strokes without
   Commit; Undo several times, Redo, toggle visibility and force re-evaluation.
2. Stroke at strength 1; set 0.5; Undo; set 1; Redo; verify both visible surface
   and persisted result after Commit and visibility toggles.
3. Repeat at a lower Sculpt Level, then across level changes; record timing.
4. Repeat Edit -> Sculpt -> Undo without Commit, including mode boundaries.
5. Confirm all-hidden native-base strokes and layer deltas remain intact.
6. Save/reload to check persistent geometry, not just a correct-looking CCG.

Expected cost: no automatic global Commit on every stroke, no full-mesh undo
snapshots, and reduced persistent capture when it cannot represent the current
surface. A step that previously refreshed must continue refreshing instead of
using an invalid coordinate buffer. Cross-frame/lower-level paths may retain
the existing global reshape cost; measure that separately.
