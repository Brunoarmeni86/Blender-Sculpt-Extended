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

- Standalone XPBD tests: **15/15 passed**, Release, Visual Studio 2022 / MSVC 19.44.
- Full Blender VS2022 compilation: **passed** with
  `WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL=ON`.
- CUDA/OptiX settings preserved from the working build: CUDA `sm_86` and OptiX enabled.
- Compiled executable:
  `C:/BlenderDev/build-se-polypaint-fill-clean-vs2022-20261115-r2/bin/Release/blender.exe`.
- Viewport deformation, native Undo, mask behavior, and visual comparison: pending user test.

The full editor build emitted only existing unused-parameter warnings in Sculpt code; no XPBD
compile or link errors occurred.

