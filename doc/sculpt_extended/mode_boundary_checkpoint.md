# Sculpt Layers: checkpoint before a mode change

Parent: `3db7a1077faa7c9257d141d668273743d8bb2bf6`.
The parent is preserved as `checkpoint/edit-mode-no-corruption-20261012`.

## Observed Windows status at the parent

The user reports no further topology corruption after the stable-corner-ID fix.
Undo still returns to the base before the brush stroke and Edit operation instead
of restoring the pre-Edit sculpt detail. This is an intermediate result, not a
validated complete Edit/Undo fix. A manual Commit has previously avoided this loss.

Delete/Fill's local fold in the supplied screenshots is native interpolation,
not the extended deformation previously produced by Undo. Fill smoothing remains
separate from this work. Main, baking, and the separate Conform UI are unchanged.

## Concrete gap in the existing source

The window-manager pre-action hook rejects operators without an UNDO flag.
OBJECT_OT_mode_set and OBJECT_OT_mode_set_with_submode have flag 0: their nested
mode operators own undo. The same hook excludes SCULPT_OT_sculptmode_toggle by
its SCULPT_OT_ prefix. The controls helper additionally requires an experimental
environment opt-in.

Sculpt-mode exit flushes changes to ID data and frees the sculpt session, but
that alone does not preserve the last stroke's persistent state before destructive
Edit operations. Corner identities can associate surviving payloads; they cannot
recover payloads that have been deleted without a stored historical state.

## Candidate

Route four mode operators through the existing pairing helper before the generic
flag/prefix exclusions. The editor helper validates native-step ownership and
global undo, commits pending changes, then pairs the active native sculpt step
with a memfile containing the current persistent state. The existing pairing
implementation keeps the native inverse stroke and preserves the redo tail.

Mode boundaries do not require BLENDER_SCULPT_LAYER_UNDO_CONTROLS. Other controls
retain the existing opt-in. The paired step is detected before commit, so nested
mode wrappers do not make duplicate snapshots. No per-stroke full snapshots or
grid rebuilds were added. One full memfile at an eligible mode boundary has a
memory/time cost to assess on large meshes.

Commit failure declines the checkpoint and is logged; it does not discard pending
data. Existing mode-exit failure handling is unchanged. If pairing fails after a
successful commit, a diagnostic is emitted; the hook does not cancel the mode
operator. This candidate does not reconstruct deleted data in already existing
undo histories: reproduce on a fresh object/history after loading the new build.

## Validation and limits

The C++ harness extracts the production WM hook, editor helper, and unchanged BKE
pairing helper. Mock services reproduce the old routing exclusion and verify
commit-before-encode, snapshot content, duplicate suppression, global-undo/stack
guards, redo preservation/barriers, and commit/pairing failure paths.

Passed with g++ C++17, -Wall -Wextra -Werror, and AddressSanitizer/UndefinedBehaviorSanitizer
(LeakSanitizer disabled due executor environment limitations).

Commit, memfile encoding, and restore are mocked. This is not a Blender compile
or native Undo test. No Blender executable or Windows toolchain is present here.

Required Windows sequence, with CCG undo enabled and old trace/control opt-ins unset:
1. Fresh Multires object and a visible recording layer. Make an unmistakable stroke.
2. Enter Edit directly through the menu, Delete/Fill a small patch, leave Edit.
3. Undo the topology/mode actions one at a time until returning to Sculpt: the
   stroke must remain when only the Edit operation has been undone.
4. One further Undo that reaches the stroke must remove it; Redo must restore it.
5. Repeat through Sculpt -> Object -> Edit and Tab, with two strokes.
6. Repeat a mode transition with a redo tail, plus strength/visibility changes.
7. Compare a manual-Commit control and check ordinary native Multires/no-layer behavior.

Do not merge into main until compiled and these native tests pass.
