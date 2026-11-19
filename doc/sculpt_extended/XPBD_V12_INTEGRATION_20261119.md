# XPBD V12 integration checkpoint (19 November 2026)

## Scope

This branch integrates the standalone XPBD V11 CPU foundation and the opt-in V12 native Sculpt Grab preview from draft PR #23 on top of the Sculpt Extended Polypaint checkpoint `dc81830fc7c`.

The integration intentionally excludes unrelated history from the R&D branch, whose base differs substantially from the current Sculpt Extended development branch.

## Safety gates

- `WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL` remains disabled by default.
- Runtime activation also requires `BLENDER_SCULPT_XPBD_GRAB_V12=1`.
- Standard Grab remains the fallback path.
- Multires Sculpt Layers, Dyntopo, deform modifiers, shape keys, real symmetry/tile passes, and meshes above the preview limit remain excluded.
- No change is made to `main`, the stable tag, or the stable build.

## Validation status

- Standalone XPBD tests: pending on the integrated branch.
- Full Blender VS2022 compilation: pending.
- Viewport, Undo, and visual behavior: pending user test.

