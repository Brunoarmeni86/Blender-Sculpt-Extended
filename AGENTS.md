# Codex instructions — Sculpt Extended experimental pre-5.4 fixes

You are working on **an experimental development branch**, not on the stable release.

**Read this first:** [Pre-5.4 handoff: remesh, gizmo-driven Symmetrize, symmetry/gizmo](doc/sculpt_extended/PRE54_SYMMETRY_REMESH_FIXES_V01_20261108.md).

## Intent and constraints
- The user wants **proposals, candidate rewrites and well-documented experiments available on GitHub** so that Codex can resume work later. **There is no requirement to solve all issues immediately.**
- The branch originates from **tag `v5.2.3-sculpt-extended-20261104`** and deliberately differs from divergent `main`. Keep stable release and `main` unchanged unless explicitly instructed.
- All initial Python patches in this branch are **WIP, untested in Blender**. Do not describe them as fixed or validated. Prefer revising a flawed prototype over building more unverified code on top.
- Preserve **Sculpt Layers, Multires, Polypaint HR, Face Sets HR, Undo, Adaptive Stroke, PBVH optimization and memory optimizations**. No new full-mesh/CCG work in per-dab hot paths.
- The user's chosen sequence is **test and stabilize current 5.2.3 first, then selectively port validated changes to 5.4**.
- Do not merge into `main` or distribute Windows/Linux binaries without testing and explicit approval.

## Tracked work
- **#12 P0** — additive Strength or extra symmetry passes when disabling combined local + reference symmetry.
- **#13 P1** — G/R/S reference gizmo, mode UI, precise surface snap and reliable anchoring/rebinding.
- **#14 P1** — mask-adaptive protected remeshing: mask=1 exactly frozen, mask=0 full remesh, partial mask controls density/influence and safe transition.
- **#15 P0/P1** — gizmo-defined Symmetrize: improve topology-copy correspondence and masked topology stitching; distinguish shape transfer from connectivity reconstruction.

## First-pass actions
1. Read the handoff memo and inspect the complete diff against the release tag; locate source-level assumptions and existing WIP code.
2. Run Python syntax checks, then the headless Blender regression script **if a suitable Blender build is available**. Never claim runtime test without actually running it.
3. Add meaningful fixtures: posed equal topology, unequal topology with protected islands, gizmo off-origin/rotated frame, all mask fractions, multiple contours, UV/Face Sets, Undo/Redo, non-manifold guard, dense meshes.
4. Isolate solutions into small auditable commits; annotate uncertain algorithmic claims and benchmarks; preserve non-destructive fallback when unsafe.
5. Report status with proof: `IDEA`, `PROTOTYPE`, `STATIC-VALIDATED`, `BLENDER-RUNTIME-VALIDATED`, `BENCHMARKED`, `STABLE-CANDIDATE`.

## Useful paths
- `scripts/startup/sculpt_extended_integrated/masked_remesh.py`
- `scripts/startup/sculpt_extended_integrated/symmetry_topology_mask.py`
- `scripts/startup/sculpt_extended_integrated/symmetry_shape_v2.py`
- `scripts/startup/sculpt_extended_integrated/symmetry_surface.py`
- `scripts/startup/sculpt_extended_integrated/symmetry_transition_uv.py`
- `scripts/startup/sculpt_extended_integrated/symmetry_transition_build.py`
- `scripts/startup/sculpt_extended_integrated/symmetry_reference_preview_transition.py`
- `scripts/startup/sculpt_extended_integrated/reference_native_snap.py`
- `source/blender/editors/sculpt_paint/mesh/sculpt.cc`
- `doc/sculpt_extended/tests/pre54_symmetry_remesh_regression.py`

The existing masked remesh is **local BMesh surface remesh**, not a volumetric voxel remesher. Do not promise volume fusion without adding a separate algorithm. The existing Multires Symmetrize transfers detail but **does not rewrite topology**; keep those operations distinct.
