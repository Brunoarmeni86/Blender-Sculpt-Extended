# Equal-level global Commit reference composition

Opt-in candidate based on 9dcf2f9; native validation pending.

The global layer Commit fallback copies the mesh, applies L0 layer offsets to
the cage and composes visible high-resolution deltas into the scratch base.
Reshape then assigns every top-level displacement from current CCG positions
before tangent conversion. Equal-level smoothing returns without using the
old reference. Thus that high-resolution sum is overwritten at equal levels.

BLENDER_SCULPT_COMMIT_SKIP_COMPOSE=1 skips only that sample sum. The flag must
be exactly 1, CCG level must equal modifier total level, and live procedural
displacement must be inactive. Keep live displacement and lower-level detail
propagation on their legacy reference paths. The standalone audit confirms
assignment writes all grid samples at equal levels; this is not a native test.

Retain the scratch mesh as a transaction boundary for masks and layer payloads.
Retain L0 cage composition, layer descriptor checks, final subtraction of the
other visible layers, strength normalization and finite validation before
publication. The local Commit remains unchanged. No Edit/Sculpt Undo fix.

Expected saving: one read/modify/write sample loop per populated effective
layer in global equal-level commits. No peak-memory saving claimed. Preparation
trace includes scratch copy, descriptor checks and composition; total Commit
also includes reshape, subtraction and dependency graph work. A single timed
manual Commit cannot establish a speedup.

Test with the Windows launcher, which forces global Commit, at maximum Sculpt
level with a populated visible layer. Check shape, Commit, Undo and Redo.
LAYER_COMMIT_REFERENCE mode=SKIP_EQUAL_LEVEL skipped_samples>0 confirms the
intended sample loop was avoided. -Off retains the same global diagnostic mode
and disables only this candidate for comparison. Keep it opt-in and reversible.
