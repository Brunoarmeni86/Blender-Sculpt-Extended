# Sculpt Extended physics brush / GPU / Multires / granular spike
**22 November 2026 — EXPERIMENTAL, NOT PRODUCTION**

Branch: `experiment/physics-brushes-warp-multires-granular-20261122`
Base: `integration/xpbd-v12-polypaint-20261119` (native V13 XPBD Grab).

## Implemented in this branch

- `experimental/sculpt_physics_cpu/brush_targets.hh/.cc`: goal generation for Grab, Clay, ClayStrips, Smooth and GranularDrag, staged for the existing XPBD `World::set_brush_targets()` API. Honors final Blender-provided mask/falloff factors, pinned particles and Standard/0% bypass. Builds sparse goals only for PBVH-selected vertices; adjacency for Smooth is cached per topology.
- `granular_contacts.hh/.cc`: local grid-hashed grain overlap correction and short-range cohesion. Emits goals without mutating mesh, UVs or solver state. This is **not** a complete DEM, MPM, fracture or friction solver.
- `multires_dirty_map.hh/.cc`: sparse inverse correspondence between physics proxy particles and evaluated Multires samples. Generation stamps identify affected samples/tiles without a full sample scan per dab; **no native CCG or Sculpt Layer writeback yet**.
- `gpu/warp_granular_probe.py`: NVIDIA Warp HashGrid / CUDA experimental Jacobi contacts. The Python/NumPy CPU oracle runs without Warp or NVIDIA. **The CUDA kernel is not compiled/tested here and is NOT called by Blender.**
- `test_brush_materials.cc`: regressions for strength semantics, modes, Clay/ClayStrips/Smooth, granular separation/cohesion and safe Multires dirty queries. CTest integration added to the existing CPU reference CMake.

## Verification limits

- The newly added C++ modules were syntax-compiled with GCC C++17 `-Wall -Wextra -Wpedantic -Werror` and smoke-tested against a minimal standalone `World` interface stub. This does **not** prove linking/behavior on the exact GitHub branch.
- The Warp Python module was syntax-checked and its CPU fallback smoke test passed. CUDA runtime / driver / device behavior **not tested**.
- Run the actual repository CTest and native Blender VS2022 build before declaring integration complete. GitHub Actions workflow on this branch attempts the actual CTest suite.
- Native V13 Grab still requires its existing compile-time flag and `BLENDER_SCULPT_XPBD_GRAB_V12=1`. Keep both OFF in stable builds.

## Quick test

```sh
cmake -S experimental/sculpt_physics_cpu -B build-sculpt-physics -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sculpt-physics -j
ctest --test-dir build-sculpt-physics --output-on-failure
python -m pip install numpy
python experimental/sculpt_physics_cpu/gpu/warp_granular_probe.py --device cpu --self-test
# CUDA only on NVIDIA host with Warp installed:
python -m pip install warp-lang
python experimental/sculpt_physics_cpu/gpu/warp_granular_probe.py --device cuda --self-test
```

## CODEX NEXT STEPS / safety gates

1. Check CTest/ASan and Windows/MSVC on this exact branch. Measure CPU local solver and contact planner p50/p95 with 100k/1M/10M vertex meshes. Validate singularities, near-zero radius, bad input, mixed pinned/masked vertices, high-degree meshes and pressure spikes.
2. Bind the **existing** native Sculpt PBVH brush callbacks to the new goal planners. Gather exactly Blender's brush factors and unique candidate IDs, and use Undo-covered nodes. Cache BrushAdjacency by topology version. Keep Standard and 0% Assist identical to the stock brushes.
3. Use the existing `SculptSurfaceBridge` for Standard/PhysicalAssist/FullPhysics; apply influence **once**. Compare with sibling branch `experiment/native-physics-assisted-brushes-20261118`, which contains an Assist-strength and NaN audit. Avoid copying old physics cores over native V13.
4. First integrate physical Grab, Clay, ClayStrips and Smooth for ordinary PBVH Mesh. Then evaluate additional types and artistic parity. Keep the physics solver isolated from unrelated brush paths.
5. Multires: bind `MultiresDirtyMap` to actual CCG grid samples and Sculpt Layer deltas, preserving non-active layers, detail-frame residuals, per-corner UV/color seams, masks, symmetry, native Undo/Redo and save/reopen. Until this passes, **keep native Multires gate disabled**.
6. Build an optional C++/Python GPU backend or native compute bridge with correct device lifetime, transfer costs and deterministic CPU fallback. Compare CPU vs Warp on the same fixtures and validate that GPU does not hurt small brush dabs. Warp's Python HashGrid sample is only a starting point.
7. Granular roadmap: stable DEM contacts, friction/tangential velocity, cohesion, plastic rest updates, breakable bonds, collision with sculpted mesh, state persistence, particle-to-surface reconstruction, UV advection, polypaint continuity, and no-NVIDIA fallback. Never imply Carnage Grains code was copied.
8. Only after CTest, editor/Multires tests, Visual Studio build and real p50/p95 measurements: enable an opt-in UI behind runtime feature flags; do not change main/stable by default.

## Main architectural risk

GPU, CCG, Sculpt Layers, Undo and real granularity require a common transaction boundary. Never write beyond the PBVH Undo coverage; stage all numerical results and validate finiteness before committing geometry changes. The current granular contact planner uses a local candidate halo and is **not** sufficient for global simulation or meshing.

## Multi-solver extension — 22 November

This branch is deliberately **not Warp-only**. New original standalone
`volume_constraints.hh/.cc` implements transactional XPBD tetra volume
correction (a generated volumetric proxy is still required before it can
deform Sculpt geometry). `solver_policy.hh/.cc` provides capability-gated
selection across XPBD surface/volume, simplified DEM and future MPM backends,
with CPU/CUDA/Vulkan device placeholders that never become enabled by default.
`test_multiphysics_volume.cc` covers conservative volume projection, fixed
vertices, validation/rollback and backend selection. This is standalone C++
reference code, **not** the final Blender editor interface.

Solver comparisons, licensing decisions, source references, material mapping
and implementation gates are documented in
[`MULTISOLVER_SOURCE_REUSE_AND_INTEGRATION_20261122.md`](MULTISOLVER_SOURCE_REUSE_AND_INTEGRATION_20261122.md).
