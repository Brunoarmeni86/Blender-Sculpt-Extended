# Edit Mode native base interpolation V0.2 candidate

Parent: `a58023c81c08b743419d5535f91387e50867fd1e`.
The user reported a working Windows build/runtime test on 2026-10-11 at 23:11 Europe/Rome,
with interpolation that could improve. This is not a claim that every regression-matrix
case passed. The user-tested parent is retained at
`checkpoint/edit-mode-windows-pass-20261011`; main remains the original stable baseline.

## Isolated change

The V0.1 topology remapper discarded native `CD_MDISPS` for every corner whose Sculpt
Layer mapping was new or ambiguous. BMesh can already supply a valid interpolated native
Multires grid for those corners. Native base displacement and per-layer deltas are separate.

This candidate retains BMesh's native grid when its payload, stored subdivision level and
sample count are valid. It only initializes a zero base grid when data are missing or the
level/count are incompatible. Newly created/ambiguous Sculpt Layer grids still contain
implicit zero delta. It does not infer correspondence for layers from interpolated base data.

Valid payloads and hidden masks are retained without allocating or copying their buffers.
The non-exit save/render flush synchronizes only the native grids actually initialized,
using the existing reset-corner bit mask.

## Validation

The standalone C++ harness runs the actual new helper plus the existing remap/free bodies:
seven prior remap cases and four additional cases (retain valid dense grid and hidden mask,
initialize missing data, replace wrong level, replace wrong sample count). ASan/UBSan enabled;
LeakSanitizer disabled for the managed sandbox. This is not Blender integration validation.

Windows compilation and visual tests are still required. Compare Delete+Fill, Extrude and
Dissolve against the checkpoint, including repeated saves in Edit Mode, REC/Commit and undo.
Verify native-base continuity improves while genuinely new layer deltas remain zero.

## Scope still pending

This first step preserves native interpolation; it does not implement local high-resolution
reprojection of each Sculpt Layer. That requires the old cage/grid coordinate mapping before
BMesh changes topology, so displacement vectors can be transported into the new tangent
frames. Blind interpolation of old tangent-space layer components would be incorrect.
