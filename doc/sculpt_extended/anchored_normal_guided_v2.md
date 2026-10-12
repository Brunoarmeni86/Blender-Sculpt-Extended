# Anchored Surface Projection V2 (experimental)

Based on `feature/anchored-surface-projection-v1` (forked from `checkpoint/edit-mode-no-corruption-20261012`).

## User controls
* Stroke Method: Anchored
* Texture Mapping: Area Plane (the primary Sculpt Texture slot, `brush.mtex`)
* Surface-Adaptive Projection: enables V1 bounded height-based correction
* Normal-Guided (Multires): additionally enables V2 for common-grid texture-factor paths

## Implementation
* `sculpt_apply_texture()` optionally accepts an evaluated per-point surface normal.
* `calc_brush_texture_factors(..., subdiv_ccg, grids, ...)` reads existing CCG normals directly from `subdiv_ccg.normals`, without building a dense per-node normal array.
* Normal processing uses the same radial and mirror symmetry transformations as the coordinate lookup.
* The local normal's radial tilt estimates `theta / sin(theta)` in the brush-local tangent frame. Projection correction is bounded to at most 1.30. Invalid/back-facing/misaligned normals fall back to V1.
* Non-grid brushes retain V1; existing standard mapping is unchanged when Surface-Adaptive is off.

## Limitations
* This is a local normal-guided approximation, not a geodesic parametrization. It cannot globally unwrap folded or self-occluded surfaces.
* Currently wired into the **common Multires factor path**; custom brush implementations may still use V1.
* No render-preview UI or geodesic distance propagation.
* Build and real Blender runtime tests are required. No full Windows build was performed during this commit.

## Manual smoke test
1. Compare Standard, V1, and V1+V2 with the same Alpha on a Multires sphere.
2. Compare side-oriented cylinder/cheek surfaces and a surface with folds (no propagation across disconnected surfaces).
3. Toggle Sculpt Layers and test Undo of Anchored stamps.
4. Validate mirrored and radial symmetry.
5. Repeat on a dense Multires mesh while measuring elapsed stroke time and peak memory.

## Regression guard
Do not merge to `main` or replace stable binaries without successful Windows compilation and visual testing.
