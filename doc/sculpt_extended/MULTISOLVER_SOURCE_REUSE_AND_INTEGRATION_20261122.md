# Multi-solver research and integration plan
**22 November 2026 — Experimental Sculpt Extended branch / PR #24.**

## Principle: use more than one physics solver
Do not treat Warp as the only solution or make a new standalone "physics mode".
The artist stays in normal Sculpt Mode with opt-in Standard / Physical Assist /
Full Physics controls. Brushes supply native PBVH-selected factors, masks, pressure
and symmetry; a material/solver policy routes the *physics proxy* through the
best verified solver; the surface bridge and transaction writer handle Mesh,
Multires CCG, Sculpt Layers, corner UVs/colors and native Undo.

### Candidate source audit (licenses checked at repository root, 2026-11-22)

| Upstream | License | Preferable reuse | Status |
| --- | --- | --- | --- |
| [PositionBasedDynamics](https://github.com/InteractiveComputerGraphics/PositionBasedDynamics) | MIT | XPBD distance, tetra volume, bending, corotational FEM, rigid/soft-body structure | Favor math/adapters; retain notices if source copied |
| [Newton Physics](https://github.com/newton-physics/newton) | Apache-2.0 (+ third-party licenses) | XPBD, VBD, MPM and Warp execution architecture | Prefer optional integration/evaluation; review GPLv3-compatible distribution, CUDA dependencies |
| [Project Chrono DEM-Engine](https://github.com/projectchrono/DEM-Engine) | BSD-3-Clause (+ third-party notices) | Broadphase, realistic DEM contact forces, friction history, complex grain shapes | Reference for a native contact backend; dependency size may be excessive |
| [VeloxSim DEM](https://github.com/VeloxSim/veloxsim-dem-open) | MIT | Warp-oriented particle contact and friction models | Candidate for small GPU kernels after numerical comparison |
| [Taichi MLS-MPM](https://github.com/yuanming-hu/taichi_mpm) | MIT | Plastic sand, snow, clay via particle-grid transfer / stress return mapping | Research + independent modular MPM backend; not a brush callback drop-in |
| [harri665 MPM](https://github.com/harri665/MPM) | MIT | Vulkan/OpenGL/CUDA/WebGPU concepts and material-per-particle models | Research alternative to CUDA |
| [Nebula](https://github.com/20niship/nebula) | MIT | Vulkan compute XPBD / granular / MPM patterns | Alternative GPU feasibility; requires independent Blender backend |
| [SPlisHSPlasH](https://github.com/InteractiveComputerGraphics/SPlisHSPlasH) | MIT | SPH, DFSPH/PBF, viscous and nearly incompressible fluids | Optional future wet mud/liquid brushes |
| [MGPBD](https://github.com/chunleili/mgpbd) | No explicit root license found | AMG/multigrid ideas for stiff/high-resolution constraints | **Research only: do not copy code without permission** |
| [NVIDIA Warp](https://github.com/NVIDIA/warp) | Check individual dependency notices | HashGrid, GPU kernels, AOT CUBIN/APIC integration | CUDA kernel probe only; CUDA not tested in PR #24 |

These license summaries are an integration screening, **not legal approval**.
Source files can contain further notices, vendored dependencies or licensing
constraints. Never bulk-copy an entire upstream tree into Blender. Respect
existing Blender GPL-2.0-or-later files and distribution obligations; audit
Apache-2.0/GPLv3 compatibility for the final linked product.

## Actual new code from this branch

- `volume_constraints.hh/.cc`: original standalone C++17 XPBD tetra volume
  projection, rest shape validation, mass-weighted gradients, compliant lambda
  corrections and transactional staging of positions and lambdas. This needs a
  **volumetric proxy/cage**: surface triangles alone do not define material
  volume. It does *not* solve shear, bending or inverted-tet robustness.
- `solver_policy.hh/.cc`: pure material-to-solver and device planning
  (Clay/Wax/Tissue -> XPBD volume when a cage exists; Sand/Snow -> DEM;
  Mud -> MPM only when actually registered, otherwise approximate DEM;
  Stone/Cloth -> surface XPBD approximations). Device GPU opt-in requires
  explicitly verified Warp CUDA/Vulkan capability and measured threshold.
  This is **not runtime dispatch**; C++ backend registration is still needed.
- `test_multiphysics_volume.cc`: tetra volume correction/pins/transaction
  regression and unsupported-backend fallback tests.
- Previous work in PR #24 remains: native V13 opt-in Grab on PBVH Mesh;
  independent target planners for Grab/Clay/ClayStrips/Smooth;
  local granular contact planner; Multires sample/dirty-tile index;
  standalone Warp CUDA proof-of-concept with NumPy fallback.

## Material-to-solver design

| Artistic goal | First useful backend | Higher fidelity candidate | Surface ownership |
| --- | --- | --- | --- |
| Grab / Clay / Wax | XPBD proxy, then tetra volume XPBD | VBD, Neo-Hookean/shape matching | Blender mesh and active Sculpt Layer |
| Dense Smooth | Coarse XPBD/Laplacian or bilateral profile | Multigrid XPBD / feature-preserving smoothing | Detail residual and CCG dirty ranges |
| Clay Strips / wood incision / stone carving | Native brush trajectory/alpha + material-specific constraints | Plastic continuum / cutting with topology transaction | Blender geometry; new topology requires UV/color transfer |
| Sand / grains | DEM hash-grid contacts | DEM-Engine contact history and polygrain collisions | Grain cache then surface reconstruction |
| Wet mud | Approximate cohesive DEM | MLS-MPM, elastoplastic stress return, coupling | MPM particles + surfacing with UV advection |
| Snow | Cohesive DEM baseline | MLS-MPM compression/hardening | Particle/grid to sculpt proxy |
| Viscous liquid | No production backend yet | DFSPH / PBF (SPlisHSPlasH) | Separate fluid surface layer, not direct Multires |
| Large stiff deformable masses | Local XPBD halo | Multigrid preconditioner/VBD | Coarse cage then high-frequency residual |

## Core integration contract

1. Native Sculpt computes samples and exact brush masks/pressure/face sets.
2. Maintain a stroke-persistent **coarse proxy** for normal Sculpt and Multires,
   with stable IDs and topology generation. Build volume tets only where
   meaningful; 1:1 mesh-vertex physics cannot scale to 20M polygons.
3. Backend API to implement: `begin_stroke`, `set_targets`,
   `solve_local`, `stage_result`, `cancel/commit`, `capabilities`.
   Non-CPU backend must not be called when unavailable.
4. CPU reference is the numerical oracle; GPU results have tolerance bounds,
   device sync, explicit buffer ownership, no implicit Python per dab.
5. No actual mesh/CCG/Undo write until the complete result is finite and
   targets are inside Undo-covered PBVH nodes. Preserve topology provenance,
   per-face-corner UVs/color, sculpt mask and nonactive layers.
6. Deformation without topology changes keeps existing corner attributes.
   Fracture/meshing changes require explicit provenance or barycentric transfer
   and reliable UV/color reconstruction; do NOT claim conservation by default.
7. For volume tets, test inverted elements, near-degenerate gradients,
   self-contact, stiff constraints, multires anchor tangents and long strokes.
   Compare XPBD residual, visual curvature, deformation volume, lag p50/p95,
   memory, Undo, and first-dab cache cost.
8. In CUDA-enabled builds, compare Warp AOT/APIC vs native CUDA execution,
   CPU baseline and Vulkan alternative on real brush regions. Optimize
   total editor latency, not just GPU kernel time.

## Code ownership / next concrete tasks for Codex

**Gate A (CPU verified):** Build the new XPBD tetra reference in MSVC, LLVM and
GCC; add ASan/UBSan and stress/inversion tests. Separate `lambda` reset between
new time steps from iterations within one step. Benchmark constraint sorting,
CSR adjacency, graph coloring vs Jacobi.

**Gate B (native PBVH):** Wire Clay and Smooth planners into native callbacks,
using Blender's own Undo and remapping rules. Integrate one solver as opt-in
for normal Sculpt; preserve exact Standard behavior and masks.

**Gate C (Multires):** Design low-res proxy cage and volume constraints with
CCG anchors; write active layer deltas only for dirty samples. Check detail
loss, normal flipping, UV seam identity and multiple Sculpt Layers.

**Gate D (GPU backend):** Implement capability and device registry, manage
resident GPU buffers, choose backend empirically, benchmark CUDA/Vulkan/CPU.
Keep stable build fully functional without NVIDIA drivers.

**Gate E (granular continuum):** Complete DEM friction, tangential history,
damping, colliders, plasticity and bond fracture, then add experimental
MPM/PBF independently and only when accuracy/budget justify it.

No automatic merges into `main`; PR remains Draft until real Blender
viewport, native Undo, Multires layer and GPU performance tests pass.
