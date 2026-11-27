# D12 dedicated brush engines — Clay and Flesh

Status: PROTOTYPE, opt-in. Base: D11 checkpoint 458d0b23870618f332b3f03f8cf04787f0b32a85. Preserve D11 and main. The user's direction is that physics is a means for artistic quality, not an end. Two brushes first. No additional generic material profiles in D12.

## Source references and license
- SculptCore (Joseph Eagar, MIT): https://github.com/joeedh/sculptcore/blob/master/source/brush/kernels/plane.sbrush (blob 79784d4febb1dd0602e697b8d088bcde047d8e4f), plane_frame.h, accum_mode.h, stroke_spacing.h, gpu_brush_session.h.
- Kelvinlet reference: https://github.com/joeedh/sculptcore/blob/master/source/brush/kernels/kelvinlet.sbrush (blob 46b4adb16d45cc79131b1f7f7eaa25d812e066eb); de Goes & James, Regularized Kelvinlets (2017), https://graphics.pixar.com/library/Kelvinlets/.
- D12 is independent code using the mathematical concepts, not imported source. If actual SculptCore code is ported later, retain the MIT copyright and permission notice from https://github.com/joeedh/sculptcore/blob/master/LICENSE.txt.
- ZBrush ClayBuildup is an artistic target, not a claim about its proprietary algorithm.

## Implementation
- d12_brush_common.hh: only host sample contract and elementary math.
- d12_clay_engine.hh: live local normal/center, spatially stabilized plane, rolled shoulder, roundness, one-sided projection, exponential arc-length response. No neighbors, iterative physics, or geodesic transport. Fixed-plane dose is partition invariant; moving-plane and variable-pressure invariance are not yet established.
- d12_flesh_engine.hh: normalized analytic regularized Kelvinlet field, tangential gesture, spatial direction memory and bounded leading-side lift/trailing depression. No stationary creep or long-gap jumps. No actual cloth/XPBD, volume conservation, or elastic rebound yet. The user wants Flow/Cloth-like organic tissue; add constraints only after artist testing demonstrates the need.
- physics_brush_v15.hh dispatches D12 before D11 material planning. Shared native sample dedup, existing compact executor, incident-triangle guard, sparse PositionDeformData/Undo and PBVH writer remain. D12 requires the local geometry guard even when the older contact flag is off. Existing stroke-owned storage is reused, and frame/gesture state is committed only after the writer acknowledges the dab.
- clay_strips.cc allows first stamp and native cursor node region for the opt-in dedicated Clay path. Candidate sets remain bounded by native gathering. This is not a new swept PBVH query. A normal/depth gate is not proof of self-intersection or opposite-sheet prevention.
- create_physical_brush_assets.py --dedicated-d12 creates three new assets in an empty separate library: D12 Clay Buildup, D12 Flesh Flow and D12 Melt Surface. Disable inherited auto smooth. Existing assets untouched. Native input, masks, alpha and pressure remain host-owned.

## Test gates
C++ tests exercise unilateral projection, masks, permission, deep/back-facing samples, normalized tip shape, fixed-plane dose split, live buildup, gesture direction, paired bulge sign, gap/no stationary creep and kernel-only timings for 1024/4096/16384 candidates. Timing is non-asserted; CI is not a Blender benchmark. D11 reference remains included.
Native host compile, asset creation, interactive strokes, Undo/Redo, density consistency and full dab timings must be verified on Windows before calling this runnable. GPU is deliberately not mandatory; no GPU dispatch in these two kernels yet. A SIMD/TBB or GPU implementation is justified only by measured end-to-end bottlenecks.

## Windows handoff
Use the existing VS2022/v143 source and incremental build paths from D11. Preserve the /Z7 workaround. Use new install prefix C:\BlenderDev\install-physics-d12-dedicated and new asset folder C:\BlenderDev\assets-d12-dedicated. No main merge.

After a successful compile and install:
```powershell
$exe="C:\BlenderDev\install-physics-d12-dedicated\blender.exe"
& $exe --background --factory-startup --python-exit-code 1 --python "C:\BlenderDev\blender-physics-v16\experimental\sculpt_physics_cpu\create_physical_brush_assets.py" -- --dedicated-d12 --output "C:\BlenderDev\assets-d12-dedicated"
```
For first ordinary Mesh test (no symmetry, modifiers, shape keys, layer recording, Multires, Dyntopo):
```powershell
$env:BLENDER_SCULPT_D12="1"
$env:BLENDER_SCULPT_LIVE_SURFACE_V30="1"
$env:BLENDER_SCULPT_SURFACE_V20="1"
$env:BLENDER_SCULPT_XPBD_OTHER_BRUSHES_V15="1"
$env:BLENDER_SCULPT_OPTIMIZE_V27C="1"
$env:BLENDER_SCULPT_STROKE_V27="1"
$env:BLENDER_SCULPT_D11_ARTISTIC="0"
$env:BLENDER_SCULPT_D11_DIRECT="0"
$env:BLENDER_SCULPT_D11_WARP="0"
$env:BLENDER_SCULPT_D11_PLASTIC="0"
$env:BLENDER_SCULPT_PROFILE_V26="0"
& $exe
```
Add the new library via Preferences > File Paths > Asset Libraries. Look for [D12 Clay CPU], [D12 Flesh CPU] or [D12 Melt CPU], not old [D11 artistic CPU]. Compare Clay Buildup against native Clay Strips on same 300k/1M mesh, strength, radius and pressure. Try stacked strokes, crossing strokes, curved/narrow surface and masked regions. Flesh: drag, turn, reverse, release and Undo. Repeat with PROFILE_V26=1 for short diagnostics only. If native compile fails or logs show no D12 path, investigate before calibrating. No automatic release or merge.

Next: tune Clay based on actual artist response; avoid serial planner/compact work if profiling indicates it dominates. Flesh local constraints/volume proxy only if analytic flow fails to produce needed shapes. Keep Multires and layer writers distinct, preserve all existing features; extend compatibility only after ordinary mesh validation.


## D12 Melt Surface — restored reference, not a phase-change simulation
The user reports the earlier Melt was particularly successful. Reuse D11's existing `relief_target(Action::Melt, Material::Mud)` and its V30 coarse paired relief transport, including curvature detrending and affine fallback. Under D12, the Mud/Melt action independently activates the fast direct D11 path even if D11_ARTISTIC and D11_DIRECT are disabled; the old material env override is ignored for this path. Native contact, local triangle guard, compact writer and Undo remain. No new thermal math, gravity, or GPU requirement. This preserves the prior algorithm, not a guarantee of matching every historical D11/V30 configuration. Direct chart/budget rejection is logged and produces no write; do not silently apply the legacy deformation on those dabs. The current inherited budget is 4096 active / 8192 patch vertices, and folded/back-facing charts may reject. Ordinary Mesh, no symmetry/modifiers/Multires for now.

For users with an existing two-asset D12 library, generate Melt only in a new folder (no need to replace Clay/Flesh assets):
```powershell
$exe="C:\BlenderDev\install-physics-d12-dedicated\blender.exe"
& $exe --background --factory-startup --python-exit-code 1 --python "C:\BlenderDev\blender-physics-v16\experimental\sculpt_physics_cpu\create_physical_brush_assets.py" -- --dedicated-d12-melt --output "C:\BlenderDev\assets-d12-melt"
```
Add the new folder as an asset library. Use the same D12 launch settings above. Recompile for the updated native dispatcher. Existing D12 asset generation enum fix preserves the Essentials direction.

### Next Melt stages: candle effect, separate mode
1. Keep Surface mode as the visual regression control; compare on bumps, convex forms, masked borders and density variants.
2. Heat field: brush-controlled heat dose, diffusion, cooling and a bounded liquid fraction. Begin with stroke-local state and explicit dwell; ensure committed geometry/state Undo before persistence. Temperature may remain normalized until calibrated material units exist.
3. Surface wax: temperature-dependent mobility/viscosity, gravity in world space transformed into object space, surface-tension-inspired flux, bounded donor/receiver budgets. Directional runoff and visible accumulation, not simply smoothing downward. A connected surface height/thickness proxy cannot model detached drops or exact enclosed volume.
4. Solidification: freeze transport as temperature falls, preserve the resulting relief. Optional post-stroke settling must have a bounded lifetime and compatible native Undo.
5. True droplet detachment and overhangs require separate volume/free-surface representation or remeshing with explicit opt-in. Do not promise topology changes from a height solver.
6. Profile CPU and GPU only after artist reference passes. Prefer persistent local buffers; avoid full mesh scans and per-dab device round trips.


## D12 Combined — selector, Clay stabilization, Melt and Very Strong
Use `tools/sculpt_extended/build_d12_combined.ps1` from the D12 branch. It preserves the previous installation, uses the existing VS2022 incremental build, verifies the existing Jacobi/A3 symbol names, generates the independent multiscale PTX/manifest with the pinned Warp 1.19.0 CUDA fixture, installs the package, checks all runtime files and creates the three dedicated brush assets in a new library. Do not copy arbitrary PTX modules from other Warp versions. A nonempty asset directory is intentionally rejected. CMake's Windows Warp packaging accepts SCULPT_WARP_PTX_DIR; warp.dll is copied from the configured pinned root.

Very Strong now has an independent ordinary-Mesh chart path selected by GPU Smooth > Very Strong. The CPU planner gathers native factors and visible interior adjacency locally, approximates area by mean squared neighbor edge length, rejects multiple writable components, boundary vertices, folded or deep charts, and subtracts a fitted affine plane. A 33x33 chart deposits weighted height residuals using bilinear splats. Fine (2 cells) and broad (5 cells) tent filters blend 25/75, with capped displacement 0.055R times native weight. Warp uses four phases in a separate module with stroke-owned compact buffers, not a full-mesh solver. CPU implements the same chart. This is a local height relaxation, not exact volumetric conservation, not a general curved-surface multigrid method. Connected holes or nonuniform topology can still bias a projected chart; reject unsafe geometry and compare on artist meshes before calibration. Triangle guard and native writer run on CPU; their latency is excluded from standalone benchmarks. Shape Preserve reduces normal motion by 80%; disable for first Strong comparison. Ordinary Mesh, no symmetry/modifiers/Multires/layer recording for the new path. Unsupported charts use the previous A3/standard fallback. Normal/Relax/Preserve Volume retain their old kernels; explicit GPU selection now has priority over the general V15 dispatch. The console reports [D12 Very Strong Warp CUDA], [D12 Very Strong CPU multiscale], or [GPU Smooth A3 CUDA]/[GPU Smooth Jacobi fallback]. Use BLENDER_SCULPT_SMOOTH_DIAG=1 for every-dab diagnostics, normally only occasional logs. GPU and native editor build/runtime still need local validation. CI tests C++ CPU reference and Python Warp CPU parity, not native CUDA ABI or Blender runtime.

Windows reference run 45906817602 passed all three suites; broad bump apex -0.055R (at safety cap), 841/3293/13045/51913 sample chart preparation and CPU filtering took 0.221/0.463/1.190/4.361 ms, respectively. Added unsaturated density fixture separately; do not claim density independence on arbitrary meshes from the capped result.
