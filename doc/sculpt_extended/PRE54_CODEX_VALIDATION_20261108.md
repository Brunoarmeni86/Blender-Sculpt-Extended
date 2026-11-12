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

### Intermittent evaluated-cache Strength doubling (2026-11-10)

The transition probe now covers a non-RNA Mesh update that evicts a dirty evaluated CCG.
Before the correction this route bypassed the layer-aware publisher, changed the native
base hash from `8cbaae51cee4c25a` to `29cee931277e0410`, left the active-layer payload unchanged
and rebuilt a doubled composed surface. The correction publishes the pending stroke first,
then refreshes only the owned Sculpt Layer payload in the already-created evaluated
Multires modifier before its imminent evaluation. Scalar modifier settings and the native
base are not copied or rewritten.

**Actually executed validation:** the expanded real-stroke transition probe **PASS** with
two spatially distinct strokes; cache-eviction surface error **0.0 max / 0.0 RMS**; native
base unchanged; active-layer payload changed; generic Multires update reconstruction
**2.39e-7 max / 1.24e-8 RMS**; Strength `1.0 → 0.5 → 1.0` restoration **6.0e-8 max**.
Visual Studio 2022 Release build, pre-5.4 symmetry/remesh fixture and functional group
probe (Undo isolated) all **PASS**. Candidate binary SHA-256:
`A2E2B56C56069FE7A16C98FD872637D1308C1ABC4DDCB06AD2A2B580EA428FBD`.

### Layer-stack scale benchmark and UI-only metadata path (2026-11-10)

The new interactive fixture `tests/sculpt_layer_scale_benchmark.py` measures 10, 100 and
500 empty Multires Sculpt Layers with Global Undo deliberately disabled, so these numbers
isolate operator/metadata scaling and do not claim to measure Undo or dense displacement
payload cost. The frozen checkpoint showed that group rename took **0.192 / 1.159 /
5.759 ms** respectively. The cause was an evaluated-modifier synchronization for data
used only by the original-ID layer panel.

The experimental follow-up skips dependency-graph synchronization for purely
organizational changes: additive list order, group membership, names and collapsed state.
Visibility changes, Strength, payload deletion and every geometry-changing path retain
their existing synchronization or rebuild. At 500 layers, group rename measured
**0.032 ms**, create **0.037 ms**, move **0.028 ms**, remove **0.041 ms** and dissolve
**0.025 ms**. Rename is about **179 times faster** than the frozen checkpoint measurement.
Collapse and expand measured **0.022 ms** and **0.019 ms**. Operations that physically
relink a single layer still scan the list: layer move improved from **10.61 to 5.28 ms**
and add-to-group from **10.55 to 5.35 ms**. Repeated creation of 500 layers remains
cumulative-linear at **1.37 s** and was not changed by this patch.

**Actually executed validation:** Visual Studio 2022 Release build **PASS**; scale fixture
10/100/500 **PASS**; group create/rename/move, layer out/in, hidden dissolve and destructive
delete probe **PASS with Undo section disabled**; complete pre-5.4 symmetry/remesh headless
fixture **PASS**. The Undo-enabled group probe still returns to the state before Multires
creation instead of its intended internal checkpoint; this remains an open fixture/Undo
problem and is not presented as a regression caused by the UI-only optimization. Candidate
binary SHA-256: `06741FF0E75AF92F4C655959F8CBAB04231D1DF5DA2FC3CB9B468660987408CF`.

### Lightweight group Undo/Redo candidate (2026-11-10)

Non-destructive layer and group operators now open a nested Sculpt Undo transaction that
stores only layer order, active-layer identity, names, Strength, visibility and group
metadata. Undo exchanges this snapshot with the current metadata, so Redo is exact without
copying high-resolution displacement payloads or creating a full Multires Memfile. The
runtime probe passed create, rename, whole-group move, moving a layer out/in, hidden-folder
dissolve, Undo and Redo. Strength/cache-eviction and pre-5.4 symmetry/remesh regressions
also passed unchanged. Destructive delete/merge operations are intentionally excluded from
this lightweight path because restoring removed payloads requires a separate owning
snapshot. Candidate binary SHA-256:
`4CE77B165FB1A457494C66F6A661F05124DD9955D14B25FAEAE68058D69EE153`.

### Destructive group-delete Undo/Redo candidate (2026-11-10)

“Delete Group and Sculpt Layers” now captures an owning snapshot of only the member
layers that are removed, including sparse high-resolution displacement and L0 payloads.
Unrelated layers are neither copied nor replaced. Undo reinserts the same payload objects
at their original indices and restores the active-layer identity; Redo detaches those same
identities back into the Undo step for exact ownership exchange.

**Actually executed validation:** clean Visual Studio 2022 Release build **PASS** (using
embedded `/Z7` debug information to bypass a local PDB-manager mismatch); group fixture
**PASS**, including destructive delete, Undo and Redo with exact row order, Strength,
visibility and grouping. Candidate binary SHA-256:
`276D60346301D95A6DA84482F78FFDF15D54E4B0102BFFCA61EC4CEEABA24CCE`.

**New user-reported open risk:** Multires masks can still intermittently disappear or be
overwritten during an as-yet-unisolated transition. This is recorded as an integrity-data
investigation, not a confirmed fix. The next probe must fingerprint mask payloads before
and after layer, symmetry, rebuild and Undo transitions before any corrective patch.

### Combined mask / group-merge Undo / Strength diagnostic (2026-11-10)

The read-only Multires audit now fingerprints the base `.sculpt_mask`, the persistent
`CD_GRID_PAINT_MASK` grids and the live CCG masks separately. A new interactive transition
probe covers mask fill, layer creation and Strength change, combined-symmetry disable,
evaluated-cache rebuild, group merge, Undo and Redo.

**Actually executed validation:** clean Visual Studio 2022 Release build **PASS**. The mask
probe completed all eight transitions. The base-mask and live-CCG hashes remained stable;
the persistent mask hash changed once when the initially dirty live mask was synchronized
during layer creation, then remained stable through combined-symmetry disable, cache
rebuild, merge, Undo and Redo. This small synthetic fixture therefore did **not** reproduce
mask loss, and no mask correction is claimed. Candidate binary SHA-256:
`780B03B80C8490651584399AE425ED3CCD9AA175FE1A6B1D7F25B814CD03756B`.

The same run confirmed a separate open defect: Merge Group changes a two-layer stack into
one layer, while the available lightweight metadata Undo requires an unchanged layer count. Undo
and Redo both reported `LAYER_UNDO_METADATA mode=SKIP reason=STACK_MISMATCH`; the merged
state remained in place. A dedicated owning replacement snapshot is required before this
operation can be called Undo-safe.

The Strength transition probe was launched against the same build but remained
**inconclusive**: brush activation was cancelled while Blender reported `Asset loading is
unfinished`. This is a probe-startup failure, not evidence of a Strength regression or a
successful Strength validation. The previously executed Strength validation above remains
the last confirmed result.

### Merge-group ownership exchange and deterministic Strength probe (2026-11-10)

Merge Group now opens a dedicated nested Sculpt Undo step. Before the destructive merge it
clones only the member layers being replaced, including their sparse HR and L0 payloads.
Undo detaches the merged result and reinserts the original member objects at their exact
indices; Redo exchanges ownership in the opposite direction. Unrelated layers are not
copied. The operation bypasses the equal-stack-size metadata restore that previously
reported `STACK_MISMATCH`.

The Strength transition probe now retries Essentials brush activation for up to 20 seconds
instead of treating Blender's normal asynchronous factory-startup asset load as a test
failure.

**Actually executed validation:** clean Visual Studio Release build **PASS**. Interactive
mask/merge probe **PASS**: merge changed 2 layers to 1; Undo restored the exact two-layer
order, active row, metadata and all three mask fingerprints; Redo restored the exact merged
state. The log reported two successful ownership exchanges and no `STACK_MISMATCH`.
This synthetic probe has empty layer displacement payloads, so dense production-model
payload restoration remains a user/fixture validation item rather than a confirmed stress
test.

The complete real-stroke Strength transition probe also **PASS**. Disabling Combine with a
pending stroke preserved the live surface and published the layer payload; cache eviction
error was **0.0 max / 0.0 RMS**; modifier rebuild error was **2.39e-7 max / 1.24e-8 RMS**;
Strength `1.0 -> 0.5 -> 1.0` round-trip error was **6.0e-8 max**. Candidate binary SHA-256:
`5B715154451C265A57D3B1C9841681A580B1F224621230752F739142C033F12C`.

### Native symmetry-reference gizmo candidate (2026-11-10)

The Sculpt reference tool now has one G/R/S implementation: the native transform operator
used by both its keymap and buttons. The older duplicate Python operator is no longer
registered. Direct and interactive transforms reject an invalid mesh attachment before
changing the reference, while the panel exposes a visible **Rebind Anchor** action beside
Detach. The operator also keeps its Move/Rotate/Size UI mode synchronized when executed
from scripts, gizmos or tests. Remaining labels in this panel were translated to English.

**Actually executed validation:** Python compilation **PASS**. The interactive Blender
runtime probe `tests/gizmo_reference_transform_probe.py` **PASS** for G translation, R
rotation, S plane-size scaling, anchored translation, rejection of a deliberately invalid
anchor and exact restoration of the 3D Cursor matrix after every operation. This is a
Python startup-tool change; the compiled executable is unchanged from the preceding
candidate. Mouse-driven snapping, axis constraints and rebind placement on a production
mesh still require the user's viewport test and are not claimed as automated validation.

### Expanded mask-adaptive remesh candidate (2026-11-10)

The density target and tangential relaxation now use the same clamped smooth-step mask
weight and the same user falloff. Previously edge splitting/collapse used smooth-step while
relaxation used a separate raw power curve, so the two phases could disagree within a
partially masked collar. Fully masked faces remain immutable and mask zero remains fully
editable; this is still a local BMesh surface remesher, not volumetric Voxel Remesh.

The regression fixture now covers mask values 0 and 1, a continuous half-mask collar, two
separate protected islands and a closed icosphere. It verifies exact protected coordinates
and topology, no growth in open boundaries or overfull edges, and a coarser density trend
toward the protected region.

**Actually executed validation:** Python compilation **PASS**; expanded headless regression
**PASS**; interactive protected-remesh operator **PASS**, changing the closed test mesh from
642 vertices / 1280 faces to 266 vertices / 528 faces while retaining both `.sculpt_mask`
and an independent point attribute. The automated Undo/Redo portion is **INCONCLUSIVE**:
`ed.undo.poll()` remained unavailable in this timer-driven Sculpt test after the topology
change. Experimental attempts to alter Undo ownership were rejected and reverted; no Undo
correction is claimed. Undo/Redo therefore remains a required manual/fixture follow-up.

### OpenVDB-enabled Voxel Remesh build (2026-11-10)

The reported `Voxel remesher failed to create mesh` message was traced to the candidate
executable being configured with `WITH_OPENVDB=OFF`. This disabled Blender's standard
volumetric Voxel Remesh backend. It also affected the protected option whenever there was no
nonzero mask to route the operation through the local mask-adaptive surface remesher.

A Visual Studio 2022 Release candidate was rebuilt and installed with OpenVDB enabled. The
existing experimental Python startup files and valid asset libraries were preserved; the
Git-LFS pointer assets present in the source checkout were deliberately not installed over
the runtime assets. The preceding executable is retained beside the candidate as
`blender.pre-openvdb.exe.bak`.

**Actually executed validation:** build and install **PASS**; runtime reports
`bpy.app.build_options.openvdb == True`; standard object-mode Voxel Remesh **PASS**, changing
the closed icosphere from 162 vertices / 320 faces to 824 vertices / 822 faces; the expanded
symmetry and protected-remesh regression suite **PASS** in the installed runtime. Candidate
binary SHA-256:
`5E3F1322CCA75B8ADAA792AF49566AC96131F622C3384044359CFD69674B91D1`.

The current timer-driven interactive operator/Undo probe is still unsuitable for a fully
headless Sculpt session: Blender has no editor Undo stack there, so invoking the native
operator in Sculpt mode reaches a null Undo stack. This is a test-environment limitation;
interactive viewport operation and Undo remain a user test and are not claimed by this
build validation.

### Mask-remesh dispatch correction (2026-11-11)

The apparent uniform whole-mesh result was not produced by the adaptive algorithm. The
native Voxel Remesh operator still required the temporary
`BLENDER_SCULPT_MASK_REMESH_V01=1` environment switch before it honored the visible
**Protect Mask** checkbox. Launching `blender.exe` directly therefore silently selected the
standard uniform OpenVDB remesher even while the checkbox was enabled.

The experimental switch has been removed. A nonzero point-domain `.sculpt_mask` plus the
checked mesh option now dispatches the protected local remesher directly; an empty mask or
an unchecked option still uses standard Voxel Remesh.

**Actually executed validation:** Visual Studio 2022 Release build and install **PASS**;
dispatch without the environment variable **PASS**, reporting 191 protected vertices and
changing the fixture from 642 vertices / 1280 faces to 277 vertices / 550 faces; the full
symmetry/remesh regression suite **PASS**. A new dense-to-coarse diagnostic confirmed
collapses and a measurable editable-to-transition density increase at voxel sizes 0.20,
0.35 and 0.50. Fully masked topology intentionally retains its original density rather than
being voxelized. Candidate binary SHA-256:
`4D141C8DA60131677ED0DB4FBA135E79248871332E1BC9982DB143ACC5570685`.

### Topology-defect quarantine for protected remesh (2026-11-11)

A production mesh triggered the safety rollback `Remeshing would introduce open boundaries
or non-manifold edges`. The local remesher pinned boundary vertices but could still
triangulate faces adjacent to pre-existing wire or overfull edges, allowing an existing
defect to multiply and correctly activating the final guard.

Open-boundary face coronas and faces incident to wire/overfull edges are now quarantined
bit-for-bit. These topology quarantines are tracked separately from mask-protected faces;
the remaining manifold region stays editable. Failure diagnostics now include the exact
before/after counts for boundary and overfull edges.

**Actually executed validation:** complete symmetry/remesh regression suite **PASS**; new
open/non-manifold fixture **PASS**, retaining exactly 33 boundary and 2 overfull edges while
remeshing the remote editable region; installed operator dispatch **PASS**, changing the
fixture from 81 vertices / 65 faces to 90 vertices / 111 faces without increasing either
defect count. Runtime script SHA-256:
`78C7DAA57E941EF4C4BFFD1B2A3F065BE9D9D258C3C8EA0C9CF5E719E9CD0874`.

### Monotonic mask-density transition (2026-11-11)

Viewport inspection exposed a density valley: the editable region used the requested voxel
target, the transition multiplied that target into an even coarser band, and the fully
masked region then jumped back to its original dense topology. The transition was therefore
not bounded by the two densities visible on either side.

Each source vertex now carries a temporary local edge-scale sample. The remesh target blends
monotonically between the requested voxel edge size and that original local scale according
to the mask. `Transition Density Scale` widens the blend but can no longer overshoot either
endpoint. The temporary scale attribute is removed before the result is committed.

**Actually executed validation:** complete symmetry/remesh regression suite **PASS**;
dense-to-coarse transition probe **PASS** at voxel sizes 0.20, 0.35 and 0.50, with every
measured middle band between the editable and original protected densities; installed
operator **PASS**, changing 2562 vertices / 5120 faces to 1461 vertices / 2918 faces and
leaving no `.local_*` attributes. Runtime script SHA-256:
`EED7E731CB0ECA1D0333A2E6A12375F8140786A51F2A437A6BD6BE4833DB8BE5`.

### Wider transition and local-density quality pass (2026-11-11)

The adaptive collar now uses a separate temporary density-influence field. It propagates
the painted mask influence outward for a controlled number of local topology steps without
altering the real Sculpt mask. This makes the blend visible across several rings even when
the painted mask edge is hard. Original local density is sampled with the median incident
edge length instead of the mean, reducing sensitivity to isolated long or short edges.

The former `Transition Density Scale` control is now **Transition Width** (`0..8` local
topology steps). It changes the physical breadth of the blend while the target remains
bounded by the editable and protected endpoint densities. Tangential relaxation continues
to project every update back to the original surface, preserving volume and details.

**Actually executed validation:** complete symmetry/remesh regression suite **PASS**;
density-transition probe **PASS** at three resolutions; installed operator with width 4
**PASS**, changing 2562 vertices / 5120 faces to 1232 vertices / 2460 faces, exposing the
new English RNA label and leaving no temporary attributes. Runtime script SHA-256:
`8FE13B6EB4CD484F0C5B4DD2CA3BA02466B5F7A806A2D18011926734E51023F5`.

### Continuous geodesic transition (2026-11-11)

Further viewport inspection showed faint concentric bands because transition width was
still propagated in discrete topology rings. The density influence is now calculated with
a scale-normalized geodesic distance over the source surface. Influence decays continuously
with physical edge length, so uneven source tessellation no longer determines visible band
boundaries. The real Sculpt mask remains untouched.

**Actually executed validation:** complete symmetry/remesh regression suite **PASS**;
three-resolution density probe **PASS**; installed operator with a deliberately hard binary
mask and width 4 **PASS**, changing 2562 vertices / 5120 faces to 1352 vertices / 2700 faces
and leaving no temporary attributes. Runtime script SHA-256:
`A64D142A02C95A0BBB3FA135B8DF8A6F053573B78D6ACB6DD779521DF1C9972C`.

### Visual Studio 2022 GPU build and remaining Undo issue (2026-11-11)

The Windows build was regenerated with the Blender-supported Visual Studio 2022 / MSVC
v143 toolchain. Cycles, CUDA 12.8, OptiX 8.0, HIP 7.2 and HIPRT were enabled. CUDA and
OptiX both enumerate the installed NVIDIA GeForce RTX 3080. HIP and HIPRT kernels were
compiled for the configured AMD architectures; runtime HIP device enumeration cannot be
validated on this NVIDIA-only workstation.

**Actually executed validation:** complete Release compilation and clean CMake install
**PASS**; installed executable startup **PASS**; Cycles build option **PASS**; CUDA device
enumeration **PASS**; OptiX device enumeration **PASS**. A second distribution build was
configured from empty build/install directories under the short `C:\BlenderDev` root after
materializing every Git-LFS asset. This avoided MSVC path-length failures seen in the long
Codex workspace and capped compiler parallelism to prevent `D8040` process-creation errors.
The installed tree contains no unresolved Git-LFS pointers; the restored splash source is a
valid PNG. Clean installed executable SHA-256:
`ED993382673FDA59136FAF4CB02A560B3702B0BF0E8D7DA9B2DB23CD327635E5`.

**Known issue:** the mask-protected remesher still does not provide a confirmed atomic
single-step Undo. A valid interactive probe showed that the first Undo can retain an
intermediate remeshed topology instead of restoring the exact pre-remesh mesh. Experimental
changes to Undo ownership were rejected and reverted. No Undo correction is claimed in
this checkpoint; this is the only confirmed release-blocking defect retained in the
candidate notes.

## Decision

**keep as experimental candidate.** The fixes are small, auditable, and covered by the
existing fixtures, but they are not `STABLE-CANDIDATE`. Non-destructive folder Undo/Redo is
now runtime-validated; destructive group-delete restoration is fixture-validated, while
destructive merge, mask integrity and production-model validation remain open. Next work
should expand mask fractions, closed meshes, multiple contours,
attributes and failure rollback before testing gizmo interaction and the broader #12/#13
matrix.
