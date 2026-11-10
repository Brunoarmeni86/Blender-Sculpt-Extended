# Pre-5.4 Codex validation — symmetry seam and masked remesh

**Date:** 2026-11-08  
**Branch input SHA:** `2a84afe89ad96acc7d42ac6712dbe47b6028ed9b`  
**Baseline tag SHA:** `v5.2.3-sculpt-extended-20261104` / `09a16d4b8482200101cb30728881ca4524edf019`  
**Scope:** experimental branch only; no change to `main`, release tag, or stable package.

## Findings and candidate corrections

### #12 — Combined OFF and reference-axis ownership

**Finding:** the non-combined C++ stroke loop obtained its pass flags from
`mesh_symmetry_xyz_get()`, then `reference_symmetry_point()` interpreted those local flags
inside the rotated reference frame. Reference-only with every local axis disabled produced
no mirrored pass.

**Candidate correction:** freeze `reference_axes` in `StrokeCache` at stroke start and use
those flags for the non-combined dab loop, Pose loop and topology automasking setup. Local
symmetry continues to use the mesh flags when the reference backend is inactive; Combined
mode continues to use its bounded, deduplicated matrix list.

**Evidence status:** **PROTOTYPE / COMPILE-VALIDATED / BLENDER-RUNTIME-SMOKE-VALIDATED**
for the C++ change. The modified sculpt targets and full Blender executable compiled on
Windows, and the regression suite ran successfully in that binary. The Python state
transition is **BLENDER-RUNTIME-VALIDATED**: with the experimental backend enabled,
Combined OFF selects native-only when a local axis is active and preserves explicit
reference-only when no local axis is active. The current headless fixture does not drive an
interactive sculpt dab, so additive Strength, Feather, geometry displacement and performance
remain unconfirmed.

**Follow-up after interactive reproduction:** the Python transition fix alone did not stop
the reported Strength multiplication when local symmetry remained enabled while Combined
was switched off. The native stroke preparation now enforces the invariant directly:
Combined OFF plus any local symmetry axis is native-only, even if a migrated file retains a
stale reference-use flag. Reference-only remains available after all local axes are disabled.
This guard is **COMPILE-VALIDATED** with Visual Studio 2022 (MSVC 19.44) and the complete
headless regression suite passes, but the exact interactive Multires Sculpt Layer sequence
still requires user validation.

**Root cause isolated after the repeated user reproduction:** changing the Object-owned
Combined setting schedules dependency-graph evaluation while the last Multires stroke is
still present only in the live CCG. Blender's legacy pre-evaluation cleanup in
`object_update_from_subsurf_ccg()` calls `multiresModifier_reshapeFromCCG()` directly. That
path writes the already composed live surface into native `CD_MDISPS`; it does not use the
Sculpt-Layer-aware commit path. Visible layer contributions can therefore be incorporated
in the native base and then evaluated again, which presents as multiplied Strength. This is
separate from the number and ownership of symmetry passes.

**Scoped candidate correction:** the experimental Combined toggle now calls
`SCULPT_OT_multires_layer_commit` before changing Object metadata when a Multires Sculpt
Layer stack exists. If the pending surface cannot be published safely to the active layer,
the symmetry change is cancelled. Plain meshes and Multires objects without Sculpt Layers
keep the metadata-only path. This avoids changing Blender's global dependency-graph cleanup
until its evaluated/original modifier synchronization can be redesigned and tested broadly.

**Evidence status:** **BLENDER-RUNTIME-VALIDATED on the automated transition fixture;
USER-MODEL-VALIDATION PENDING**. The fixture executes a real Draw stroke with local X plus
reference Y symmetry, leaves coordinates pending, disables Combined through the same UI
operator and fingerprints the three storage domains. Before the correction the toggle
cleared `pending_coords`, changed `native_hash`, left the layer hash unchanged and preserved
the live surface only because that same surface had been baked into the base. After the
correction the native hash stays unchanged, the active-layer hash changes, and the live
surface hash is identical before and after both the toggle and dependency-graph update.

### #15 — topological seam seed

**Finding:** `symmetry_shape_v2.topology_pairs()` referenced `ordered`, a local variable of
its caller, instead of its `coordinates` argument. The first Blender runtime execution
failed with `NameError` before evaluating the posed-seam fixture.

**Candidate correction:** use `coordinates` consistently for seam-plane and face-side
classification.

**Evidence status:** **BLENDER-RUNTIME-VALIDATED for the included equal-topology posed-seam
fixture only**. This does not validate unequal topology, multiple contours, Multires,
interactive gizmos, Undo, or production-scale meshes.

### #14 — mask-adaptive surface remesh

**Finding:** the WIP remesher checked exact protected coordinates/faces, but did not reject
growth in boundary edges or edges shared by more than two faces before publishing.

**Candidate correction:** record those two topology counts before remeshing, reject growth,
return the final counts in diagnostics, and assert them in the 7x7 mask fixture.

**Evidence status:** **BLENDER-RUNTIME-VALIDATED for the included open-grid fixture only**.
The algorithm remains a local BMesh surface remesher, not a volumetric voxel remesher.
Self-intersection, attributes, dense meshes, multiple protected islands, and performance
are not validated by this run.

## Tests actually executed

1. `py_compile` on the five changed startup modules and the regression fixture: **PASS**.
2. First Blender run, stable 5.2.3 package: **FAIL**, `NameError: ordered is not defined`.
3. Second Blender run after the candidate correction, with
   `--python-exit-code 1`: **PASS**.
   - `PASS: topological seam on strongly deformed/posed halves`
   - `PASS: masked full-face protection with adaptive density`
4. `git diff --check`: **PASS**.
5. #12 state fixture on the 4 November portable build with
   `BLENDER_SCULPT_SYMMETRY_REFERENCE_V01=1`: **PASS**. This loads the candidate Python
   callback directly from the worktree; it does not execute the unbuilt C++ candidate.
6. Windows CMake build at candidate commit `a43ea33e7158`: **PASS**.
   - `bf_editor_sculpt_paint`: **PASS**; modified `sculpt.cc`, `sculpt_pose.cc` and
     `sculpt_automasking.cc` compiled without errors.
   - full `blender` target: **PASS**.
7. Candidate binary startup/version check: **PASS**. Blender reported `5.2.3 LTS`, hash
   `a43ea33e7158`, build timestamp `2026-11-08 17:04:41`.
8. Full headless fixture on the candidate binary with
   `BLENDER_SCULPT_SYMMETRY_REFERENCE_V01=1` and `--python-exit-code 1`: **PASS**.
   This validates loading and smoke execution of the compiled candidate, but is not an
   interactive stroke-strength measurement.
9. Follow-up native guard build with Visual Studio 2022 Build Tools 17.14 / MSVC 19.44:
   **PASS**. The full executable and Sculpt target compiled; the same headless fixture passed
   on hash `4f7067ca02d8` plus the uncommitted native guard. No release package was modified.
10. Targeted real-stroke transition probe before the scoped commit: **REPRODUCED**.
    - pending live CCG position hash changed after the stroke;
    - disabling Combined cleared the pending flag;
    - native Multires hash changed from `8cbaae51cee4c25a` to `58aadf44fa3c755f`;
    - active Sculpt Layer grid hash remained `c84586b9c4ccf7ba`.
11. The same probe after committing before the Object-property transition: **PASS**.
    - native Multires hash remained `8cbaae51cee4c25a`;
    - active layer grid hash changed from `c84586b9c4ccf7ba` to `c5c1679a85cf90d2`;
    - live surface position hash remained `17819917386554150618` across immediate toggle,
      dependency-graph update and explicit follow-up Commit.
    The fixture is `doc/sculpt_extended/tests/symmetry_strength_transition_probe.py`.
12. Checkbox transition follow-up on the Visual Studio 2022 candidate: **PASS**.
    The custom push-button was removed and `combine_local` is again drawn as a normal RNA
    Boolean checkbox. Its update callback performs the same guarded pre-commit before
    dependency-graph evaluation. On the real-stroke fixture, disabling with pending
    coordinates took **8.48 ms**, clean enabling **0.10 ms**, and clean disabling
    **8.58 ms**. These are small-fixture timings, not production-scale benchmarks.
13. Sculpt Layer group behavior probe: **PASS for create, rename, whole-group move,
    layer move out/in, hidden-state restoration and non-destructive dissolve**. Four layers
    remained present with the same names, strengths and individual visibility states after
    the folder was dissolved. The first Undo/Redo extension required reacquiring the RNA
    modifier after Blender restored the datablock; the corrected Undo/Redo extension was
    not rerun because manual testing was handed to the user.
14. Visual Studio 2022 full candidate build after the checkbox/group changes: **PASS**.
    Candidate executable SHA-256:
    `AF0F3F9828D1131D3880A9EA2E181AFF49DFDC3C6A755652FDDD3BDA4874530A`.
    The full pre-5.4 symmetry/remesh headless fixture also passed on this binary.

## Sculpt Layer group candidate (2026-11-09)

Groups remain lightweight organizational metadata on existing Sculpt Layer rows; no DNA
layout or `.blend` format migration was introduced. The candidate adds:

- rename propagated to every layer in the group;
- whole-folder up/down movement without splitting its contiguous layer block;
- direct movement of a layer into a group or out of it;
- folder dissolve that removes only group metadata and retains every layer/payload;
- restoration of each member's saved visibility when a hidden folder is dissolved;
- matching controls in the Multires modifier and Sculpt Layers sidebar.

The previous destructive **Merge Group** backend remains available to code, but its header
icon was replaced by the non-destructive dissolve action to avoid confusing folder deletion
with payload merging. Literal Outliner drag-and-drop is not implemented in this compact
panel candidate; membership and ordering use explicit controls with the same non-destructive
container semantics.

The earlier baseline-package run reported build timestamp `2026-11-01 15:09:11` and
`hash unknown`. No benchmark was executed.

## Sculpt Layer organization performance and destructive folder delete (2026-11-09)

The user reported that folder and general layer operations had become noticeably slow. A
source audit found three avoidable global paths in otherwise organizational operations:

- folder create/add/remove/dissolve could destroy the PBVH/Subdiv CCG even when effective
  layer visibility did not change;
- metadata-only operations sent `ND_MODIFIER`, scheduling complete Multires evaluation;
- editing a Sculpt Layer name reused `rna_Modifier_name_update`, which also rebuilt
  dependency-graph relations.

The experimental candidate now commits only when live CCG coordinates are genuinely
pending, sends metadata synchronization plus UI redraw for geometry-neutral changes, and
uses the geometry rebuild fallback only when effective visibility/composition changes.
Layer-name updates no longer rebuild modifier relations. The optimized paths cover layer
and whole-folder reorder, create, rename, collapse/expand, membership changes, and folder
dissolve. No per-dab Sculpt path was changed.

Two distinct folder removal actions are now exposed:

- **Dissolve Group** (`X`): remove folder metadata and keep all contained layers/payloads;
- **Delete Group and Sculpt Layers** (trash): remove the folder and every contained layer,
  after an explicit warning/confirmation dialog.

The destructive backend selects the nearest surviving row and discards the live CCG only
when a deleted layer contributed to the visible surface. Hidden-only content deletion uses
the metadata path.

**Actually executed validation:**

1. Visual Studio 2022 full executable build: **PASS**, no new compiler errors. Candidate
   executable SHA-256: `F2E667923CFA30C28348E2087AB4D1B6FD2445BEA80481764B8EEA4F8C53702D`.
2. Python `py_compile` for the sidebar, group probe and translated Sculpt Extended modules:
   **PASS**.
3. Functional four-layer runtime probe with Undo isolated: **PASS** for create, rename,
   whole-group move, moving a layer out/in, hidden-folder dissolve with individual
   visibility restoration, and delete-folder-with-contents. The destructive alternative
   retained only the two layers outside the deleted folder.
4. Small-fixture operator timings (not a dense production benchmark): create **0.293 ms**,
   rename **0.165 ms**, whole-group move **0.171 ms**, hidden-group dissolve **1.677 ms**.
5. Full pre-5.4 headless symmetry/remesh fixture on the candidate executable: **PASS**.
6. Folder Undo/Redo extension: **FAIL / OPEN**. `Undo` returned to the previous global
   checkpoint and removed the test Multires modifier instead of restoring the folder state.
   Reacquiring the modifier by type confirmed this was not a stale Python RNA reference.
   Therefore organizational behavior and small-fixture latency are runtime-validated, but
   the candidate is not Undo-validated and must remain experimental. A dedicated lightweight
   layer-stack Undo transaction is preferable to reintroducing a full Multires/memfile
   checkpoint on every organizational click.

## English UI translation resumed (2026-11-09)

User-visible Italian labels, tooltips, status messages and guarded error reports in the
Multires sidebar and the current symmetry reference, snap/pivot, protected remesh,
Multires-detail and topology-stitching modules were translated to English. Internal enum
identifiers and persisted property names were not changed. The topology-fallback string
match was updated together with its translated exception to preserve behavior. Static
Python compilation and the complete headless regression passed after these edits.

## Strength multiplication and transition latency audit (2026-11-09)

The real-stroke fixture exposed two independent dependency-graph hazards behind the
reported Strength multiplication:

1. `combine_local` and `reference_axes` were still Python `PropertyGroup` values. Even a
   clean checkbox transition dirtied Object state and caused a complete Multires
   evaluation (about 8–12 ms on the small fixture).
2. Any ordinary Multires RNA update could reach `object_update_from_subsurf_ccg()` while a
   stroke was still pending. A runtime reproduction using the Multires `quality` property
   changed native `CD_MDISPS` from `8cbaae51cee4c25a` to `29cee931277e0410`, left the
   Sculpt Layer payload unchanged and changed the composed surface. This is a second,
   confirmed route for baking an already composed layer into the base and applying it
   again during evaluation.

The experimental candidate moves Combined and reference-axis routing into native Object
DNA/RNA metadata with `PROP_NO_DEG_UPDATE`. The native Combined setter commits only when
the live CCG actually has pending coordinates; clean toggles and axis selection are
metadata-only. A separate routing-valid marker migrates existing experimental `.blend`
files from their legacy Python settings instead of interpreting newly repurposed padding as
configured axes. The common Multires RNA geometry-update callback now performs the same
pending-coordinate guard before scheduling dependency-graph evaluation. A failed commit
cancels the evaluation and reports an error instead of reshaping the native base.

**Actually executed validation:**

1. Full Visual Studio 2022 executable build: **PASS**. Candidate executable SHA-256:
   `2AC94ADD05B3B28587601554FCF1819446665DA5B72C3A2BCE6B88F4D03DC38F`.
2. Real Draw stroke, local X plus reference Y, Combined ON → OFF: **PASS**. The live
   surface remained byte-identical, the active layer payload changed, the native base
   stayed unchanged and no subsequent dependency-graph evaluation altered the result.
3. Generic Multires `quality` update with a second pending stroke: **PASS after the common
   guard**. The layer payload changed, pending coordinates cleared and native base hash
   remained `8cbaae51cee4c25a`. The CCG rebuild differed from the pre-rebuild surface by at
   most `2.39e-7` per coordinate (`1.24e-8` RMS), consistent with packed
   encode/evaluate rounding rather than doubled displacement.
4. Clean Strength `1.0 → 0.5 → 1.0`: **PASS**. The half-strength surface changed and the
   restored surface differed by at most `6.0e-8` per coordinate (`9.72e-10` RMS).
5. Small-fixture setter timings (not dense production benchmarks): Combined OFF with a
   pending stroke **0.158 ms**; clean Combined ON **0.006 ms**; clean Combined OFF
   **0.002 ms**; clean reference-axis change **0.004 ms**; guarded generic Multires update
   assignment including the local pending commit **0.115 ms**; clean Strength to half
   **0.282 ms** and restore **0.153 ms**. The deliberately requested Multires rebuild
   itself remained about **4.75 ms** and is not included in the assignment timing.
6. Complete pre-5.4 symmetry/remesh headless fixture on the new binary: **PASS**.

The fixture now exports exact CCG coordinates, layer hashes and native-base fingerprints;
it distinguishes byte-hash changes caused by floating-point reconstruction from visible
Strength multiplication.

**Open risk, not claimed fixed:** animation synchronization intentionally bypasses the
interactive Strength setter transaction while Blender is copying evaluated animation back
to the original datablock. A frame change with a pending Sculpt stroke has not yet been
runtime-validated. Likewise, non-RNA callers that schedule Multires evaluation without the
common modifier update callback still require targeted coverage. These are audit findings,
not confirmed regressions in this candidate.

### User-model validation and frozen checkpoint (2026-11-10)

The user repeated the reported production sequence on a large real model and confirmed
that Combined symmetry, normal symmetry and Sculpt Layer Strength now appear to work
correctly. Performance on the large mesh was reported as acceptable, although potentially
improvable. This upgrades the primary #12 transition from automated-fixture-only evidence
to **USER-MODEL-VALIDATED** for the reported workflow; it is not a general performance
certification.

The validated source and binary are frozen as the experimental checkpoint
`checkpoint/pre54-strength-validated-20261110`. Binary SHA-256:
`2AC94ADD05B3B28587601554FCF1819446665DA5B72C3A2BCE6B88F4D03DC38F`.
Future performance work must compare against this checkpoint and preserve its base/layer/
surface fingerprint tests. No stable tag, release package or `main` branch was changed.

## Decision

**keep as experimental candidate.** The fixes are small, auditable, and covered by the
existing headless fixtures, but they are not `STABLE-CANDIDATE`. In particular, folder
Undo/Redo remains open and must not be described as fixed. Next work should implement and
validate lightweight layer-stack Undo, run dense 10/100/500-layer timing, then expand mask
fractions, closed meshes, multiple contours, attributes and failure rollback before testing
gizmo interaction and the broader #12/#13 matrix.
