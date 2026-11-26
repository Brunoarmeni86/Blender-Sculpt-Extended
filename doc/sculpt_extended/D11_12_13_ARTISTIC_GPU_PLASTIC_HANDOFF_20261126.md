# D11.12–13 artistic pressure GPU and persistent compaction
Status: experimental, no merge. Base runnable checkpoint before this work: 2d8a61f7950a. Fast CPU reference checkpoint: eeeee4807d36.

## What is implemented
- D11.12: the Warp pressure kernel computes per-vertex local motion and scalar transport drive for Press, Compress and Compact. C++ owns a bounded local device session per stroke, using the existing shared Warp context/module infrastructure. It uploads only the current local patch and reads back local proposals. CPU still computes geodesic flux and performs geometric acceptance. No GPU coarse relief transport or editor writes. This is a parity candidate, not proof of speedup.
- D11.13: reuse .bse_plastic_compaction POINT/FLOAT. Material-specific permanent compaction depends on accepted inward motion, brush radius and plastic fraction; native Undo snapshots must succeed before attribute mutation. Only successful artistic pressure targets select the new law; legacy fallback retains legacy behavior. Existing compaction reduces response of pressure, deposition, scrape and relief transport. No true elastic rebound, deformation tensor, or volumetric strain. No creation of attributes inside a stroke.

## Enable only for experimental testing
Preserve the VS2022/v143 incremental build and previous install. Check the full native build before any artist testing. The normal artistic flags from the preceding handoff remain required. Add:
```powershell
$env:BLENDER_SCULPT_D11_FAST = "1"
$env:BLENDER_SCULPT_D11_PLASTIC = "1"
# Only after generating the correct PTX using the same Warp version as native:
$env:BLENDER_SCULPT_D11_WARP = "1"
```

Generate the pressure PTX on a CUDA-capable PC using the pinned Warp 1.19.0 environment and the same CUDA ABI as the native bridge. From repo root, use the Python interpreter for that environment:
```powershell
python experimental/sculpt_physics_gpu/prototype_warp_artistic_pressure.py --ptx-dir C:\BlenderDev\ptx-artistic-d1112
```
The script prints the PTX path and actual generated kernel symbol as PowerShell assignments. Apply those in the launch shell, along with the existing BLENDER_SCULPT_WARP_PTX for Jacobi (the shared runtime still requires it) and the existing runtime DLL setup. A missing/mismatched module must fall back to CPU. Do not copy the kernel symbol from a different Warp build.

D11.13 requires the existing .bse_plastic_compaction point float attribute. Create it outside Sculpt Mode only, for a disposable ordinary Mesh test object, if absent. From Blender Python Console in Object Mode:
```python
obj = bpy.context.object
assert obj is not None and obj.type == 'MESH' and obj.mode == 'OBJECT'
a = obj.data.attributes.get(".bse_plastic_compaction")
if a is None:
    a = obj.data.attributes.new(name=".bse_plastic_compaction", type='FLOAT', domain='POINT')
assert a.domain == 'POINT' and a.data_type == 'FLOAT'
```
Save a separate test copy. Without the attribute, artistic compaction stays disabled. No Multires, shape keys, deformation modifiers or symmetry in this preview.

## Acceptance gates
1. Standalone C++ and Python Warp parity fixtures on Linux and Windows; distinguish CUDA skip from execution.
2. Native VS2022 compile, headless startup, GPU module loading with the pinned Warp runtime and generated PTX.
3. CPU/GPU A/B with identical stroke, pressure, masks, and mesh; compare motion, first-dab and warm latency. Confirm the log says Warp local + CPU transport before claiming GPU execution. Pressure local kernels may be too small to accelerate the total pipeline; keep CPU if not faster.
4. Verify compaction attribute changes only after accepted writes, persists across strokes, and is restored by Undo/Redo alongside positions. Test same region before/after compression, and compare Flesh/Foam to Firm Clay. No claim of actual elastic rebound.
5. Next major performance milestone: GPU brush-scale relief transport, sparse per-vertex writeback, and a measured end-to-end budget. Do not enable by default or merge main.

CI does not build the full Blender host or native Warp bridge. Do not label the checkpoint runtime-validated based on CI alone.


## D11.12b — paired exchange and sparse native writer

Added the optional `PairedFluxBackend` contract and Warp exchange kernel. CPU still prepares the contact chart, residual fits, interpolation stencils and signed edge proposals; Warp calculates simultaneous donor/receiver demand, applies capacity limits and accumulates equal/opposite volume exchanges on coarse chart and fine residual edges. Small edge sets (<256), unavailable modules, invalid output and device failures use the reference CPU implementation. The fallback is checked for finite results, zero pinned output, signed balance and per-node net capacity. These checks do not prove exact float parity, collision prevention, or physical mass conservation. Atomic accumulation can differ slightly by scheduling.

Generate both new PTX modules in the pinned native Warp 1.19.0 environment:
```powershell
python experimental/sculpt_physics_gpu/prototype_warp_artistic_pressure.py --ptx-dir C:\BlenderDev\ptx-artistic-d1112
python experimental/sculpt_physics_gpu/prototype_warp_artistic_flux.py --ptx-dir C:\BlenderDev\ptx-artistic-d1112
```
Apply the actual four environment assignments printed by the two scripts. The old Jacobi PTX and matching Warp DLL are still required by the shared runtime. Confirm actual logs:
- `[D11 artistic Warp local + CPU transport]`: pressure local stage dispatched.
- `[D11 artistic Warp flux exchange + CPU chart]`: paired volume exchange dispatched.
- `[D11 artistic CPU]`: no successful GPU stage.
The exchange is partial GPU offload, not a fully resident GPU solver. GPU timings may be slower because chart construction and transfers remain; compare warm end-to-end times and disable Warp if it loses.

Sparse writer: under BLENDER_SCULPT_D11_FAST=1, on supported autonomous ordinary meshes without mirror clipping, the existing native PositionDeformData writer receives only unique changed indices and corresponding translations. Native Undo coverage remains mandatory; PBVH bounds are refreshed over the original region after all writes and position invalidation is unchanged. No writes to untouched vertices; with mirror clipping the dense path is retained. The candidate scan, local geometry guards and native PBVH updates remain CPU. Validate mask/axis lock/Undo and shared-boundary bounds in actual Blender. Do not equate this with a sparse GPU viewport update.

Before enabling compaction, test CPU Fast path then GPU on identical meshes; keep profiling off for subjective latency tests and enable it for short matched diagnostic strokes. Compare V26 plan/write times, V30C staged/moved counts, D11 actual backend and displayed shape. Full native compile, PTX generation, CUDA execution and Undo/Redo need Windows runtime validation. Standalone CI cannot certify the native writer or C++ CUDA ABI.
