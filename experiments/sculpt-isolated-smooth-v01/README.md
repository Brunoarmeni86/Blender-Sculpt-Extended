# Sculpt Isolated Smooth V01 (EXPERIMENTAL)

Branch: feature/sculpt-isolated-smooth-v01; base: fix/layer-aware-ccg-writeback-v01.
Conform, Bake and production/main remain unchanged.

## Concept
Ordinary Smooth reads the composed CCG and therefore can visually cancel wrinkles, scales and pores stored on another Sculpt Layer. The opt-in test instead averages virtual positions Pwork = Pcomposed - sum(other effective layer contributions transformed into object-space tangent frames). It applies only Smooth(Pwork)-Pwork to the visible CCG. Normal active-layer Commit persists these new coordinates; other layers are not written. Reads include neighboring grids using a callback, and only a per-worker sparse offset cache is allocated; no full second CCG.

## Activate
Set BLENDER_SCULPT_SMOOTH_ISOLATED=1 before launching the experimental Blender binary. Set 0 or unset to get original Smooth. No GUI control yet.

## Current restrictions
- Top Sculpt Level = Total Level; CCG grid size >= 9.
- Active visible Sculpt Layer, REC on, with other visible effective layers.
- No procedural displacement or Shrinkwrap.
- Does not support other layers with coarse L0 delta payloads.
- Smooth Laplacian only, not Smooth Preserve Shape or Surface Smooth.
- For V01 seam safety, two samples near every CCG grid border are not smoothed. This is a temporary limitation, not acceptable for release.
- Brush falloff, masks and normals still use composed geometry.
- Unsupported states silently fall back to standard Smooth.

## Build and test
Use Git worktree C:\BlenderDev\blender-sculpt-isolated-smooth-v01 and SEPARATE build directory C:\BlenderDev\build_windows_x64_isolated_smooth_v01. Configure using the working Blender/Visual Studio 2022 CMake environment. Do not reuse your existing Release build.
With same .blend and same stroke, compare flag OFF and ON on a small 2-layer test: layer A holds high-frequency wrinkles, layer B is active REC. Measure hashes/positions of A, base MDisps and B before/after, plus the visible difference. Repeat at Strength 0.5, with Undo/Redo and Save/Reopen. Do not test on the production file.

Status: C++ prototype committed; not compiled or tested in Blender on Windows. Source-only test at this stage.