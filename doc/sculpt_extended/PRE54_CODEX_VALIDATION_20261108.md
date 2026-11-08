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

**Evidence status:** **PROTOTYPE / STATIC-VALIDATED** for the C++ change. The Python state
transition is **BLENDER-RUNTIME-VALIDATED**: with the experimental backend enabled,
Combined OFF selects native-only when a local axis is active and preserves explicit
reference-only when no local axis is active. The modified C++ has not yet been compiled
into a Blender binary, so additive Strength, Feather, geometry displacement and performance
are not validated.

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

Blender reported `5.2.3 LTS`, build timestamp `2026-11-01 15:09:11`; the binary printed
`hash unknown`. No benchmark was executed.

## Decision

**keep as experimental candidate.** The fixes are small, auditable, and covered by the
existing headless fixtures, but they are not `STABLE-CANDIDATE`. Next work should expand
mask fractions, closed meshes, multiple contours, attributes and failure rollback before
testing gizmo interaction and the broader #12/#13 matrix.
