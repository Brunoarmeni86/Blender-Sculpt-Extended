# Sculpt physics GPU prototype

Status: **native experimental integration**, compiled but not yet validated by
an interactive Blender sculpt test.

The first CUDA algorithm uses a vertex-centric Jacobi shape constraint rather
than updating raw edge constraints in traversal order.  Every neighbour predicts
the vertex position from its rest edge vector; iterations use separate input and
output buffers.  This gives deterministic parallel behaviour and avoids exposing
mesh-edge ordering as visible corrugation.

The brush-facing interface is deliberately independent from CUDA and Blender
editor state. It accepts particles, bonds and exact brush targets, making the
same contract reusable by Grab, Snake Hook, Pose and Elastic-style brushes.
Blender remains responsible for masks, symmetry, layer semantics and Undo.

## Backend policy

| Hardware | Current path | Policy |
|---|---|---|
| Any CPU / AMD / Intel / NVIDIA | CPU reference | Safe default and automatic fallback |
| NVIDIA CUDA | Warp CUDA Jacobi | Experimental, explicitly selected only |
| AMD | HIP | Extension point; no kernel or validation yet |
| Cross-vendor | Vulkan compute | Extension point; no kernel or validation yet |

Warp CUDA is not a portable GPU implementation. A future HIP or Vulkan backend
must implement the same `BrushSolverBackend` contract and pass CPU parity tests;
it must not add vendor-specific decisions to individual brushes.

The packaged NVIDIA implementation should contain PTX variants covering the
supported Blender/CUDA architecture range and select a compatible module from
the detected device architecture. A missing/incompatible module must fall back
to CPU. Until this packaging and interactive parity work is complete, `Auto`
stays on CPU and CUDA requires explicit selection.

## Required validation before enabling Auto GPU

- continuous drag and large-radius strokes, not only isolated dabs;
- mask, symmetry, Sculpt Layers, Undo/Redo and stroke cancellation;
- finite output and bounded displacement after device/module failure;
- CPU/GPU behavior comparison for all exposed physical controls;
- warm timings separated from PTX compilation/module loading;
- at least two NVIDIA architecture generations, followed separately by any
  future AMD/Intel backend tests.

Run with Warp 1.19 on the repository-pinned environment:

```powershell
$env:PYTHONPATH = 'C:\BlenderDev\third_party\warp-1.19.0'
& 'C:\BlenderDev\blender\lib\windows_x64\python\313\bin\python.exe' `
  .\experimental\sculpt_physics_gpu\prototype_warp_jacobi.py
```
