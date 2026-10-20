# Recovered verified Windows source chain — 20 October 2026

This commit consolidates Layer Region V01, Sculpt Destination V01, Strength Zero
Fix V01, Subdivide Regional Execute V03 and Bake Visible Hidden V01 on top of
`be08f88ab9941ad4e7052e2114961deefa29a281`. The existing shared Polypaint controls
are preserved and combined with the verified native Multires sidebar and Draw
Transport property lifecycle. No Face Sets algorithm or data format is changed.

The recovered Windows report is committed alongside this note. It certifies
the existing executable SHA256
`da235eb28ff27873414ca095104bf325e11a04413f6ac63cae36ba76c8cd9f5d`,
with HR/L0 Bake Visible, hidden layer preservation, cancellation when all layers
are hidden, repeated bake, Undo/Redo and save/reopen. These are recovered Windows
results, not a new compilation in this session.

The combined UI passes 13 focused Python checks; the actual Blender lifecycle
check is skipped outside Blender. A visual check on Windows is still needed.
The actual bake/merge body harness passes AddressSanitizer/UndefinedBehaviorSanitizer
with leak detection disabled because this container cannot inspect /proc tasks.
This harness uses stand-in Blender types, not a full Blender build.

Regional V03 remains opt-in with `BLENDER_SCULPT_SUBDIVIDE_REGIONAL_V03=1`.
Shared-Work topology/base caches remain OFF. Keep
`BLENDER_SCULPT_COMMIT_KEEP_CCG=0`. Launch profiles are retained in the
reproducible test kits; this commit does not enable every experimental flag as a
native default or claim a general performance multiplier.

The old native_release kit is a historical snapshot and must not be replayed
over current sources. Likewise the Bake V01 test kit has its original hash
guards and is retained for reproducibility, not deployment on this newer tree.
The Windows shareable sources may have been restored by earlier test runners;
use this committed chain to rebuild the consolidated candidate.

Next work: rebuild the combined candidate, check its UI on Windows, then profile
subdivision phase times and peak memory in a separate experimental version.
