# Blender open-source technology radar

**Last researched:** 2026-11-17. **Scope:** GitHub research across Blender add-ons, native geometry libraries, 3D vision, physics, procedural tools, pipeline and automation.
**This is research, NOT a development schedule or a claim that any of these tools works with our custom Blender build.**

## Status and selection rules

- P0/P1/P2 are **research priorities**, not implementation priorities or commitments.
- `code` = relevant source file inspected; `README` = behavior checked in upstream project documentation; `metadata` = public repository metadata was checked, technical claims remain to validate; `proposal` = unverified candidate.
- A repository being active does not establish quality, correct behavior, compatibility, safety, or licence to redistribute bundled engines.
- License tags are from GitHub metadata or top-level documentation. **Check actual LICENSE, submodules, bundled binaries, datasets, models and individual files before copying or distributing anything.** 'Review files' and 'No licence found' are NOT permission to reuse.
- Repository `last_push_checked` refers to the code push timestamp at check time, **not the last GitHub page update**. Dates can change. Unknown means not verified.
- Keep the existing Sculpt/Multires, photogrammetry and Facewrap implementations as authoritative; do not add a duplicate architecture just because a similar GitHub project exists.
- Test candidates on copied meshes and disposable scenes; check output geometry, deterministic behavior, memory, elapsed times, Undo, save/reopen, layer preservation and Blender 5.2/5.4 compatibility.

## Catalog (46 distinct verified repository endpoints)

| Research | Area | Repository | Interesting component | License/redistribution gate | Last code push | Evidence |
|---|---|---|---|---|---|---|
| P0 | Retopo / geometry | [CurioCrafter/CurioMesh](https://github.com/CurioCrafter/CurioMesh) | Quad retopo region-aware; Rust field/UV pack; reprojection/quality tests | GPL-3.0 | 2026-11-11 | code |
| P0 | Scan repair / bake | [shaderko/remi-blender-addon](https://github.com/shaderko/remi-blender-addon) | Patch-based repairs, alpha-wrap, MeshLab, UV/bake transaction | GPL-3.0 | 2026-11-10 | README |
| P0 | Projection / 3DGS | [gokayfem/gaussian-splat-to-blender-nodes](https://github.com/gokayfem/gaussian-splat-to-blender-nodes) | Multiview visibility depth test, weighted radiance blend, material-specific fallback | MIT (repo) | 2026-10-24 | code |
| P0 | Texture / layers | [ucupumar/ucupaint](https://github.com/ucupumar/ucupaint) | Shader-space texture layer stack, compositing and painting UX | GPL-3.0 | 2026-11-16 | metadata |
| P0 | UV / geometry | [jpcy/xatlas](https://github.com/jpcy/xatlas) | UV atlas generation / parameterization library | MIT | 2024-06-16 | metadata |
| P0 | Solver / procedural | [BradyAJohnston/MolecularNodes](https://github.com/BradyAJohnston/MolecularNodes) | XPBD distance constraint node builder; Geometry Nodes simulation patterns | GPL-3.0 | 2026-11-16 | code |
| P1 | Volume / topology | [AcademySoftwareFoundation/OpenVDB](https://github.com/AcademySoftwareFoundation/OpenVDB) | Sparse volume representations; level sets and surface reconstruction | Apache-2.0 | 2026-11-09 | metadata |
| P0 | SfM / MVS | [colmap/colmap](https://github.com/colmap/colmap) | Camera registration, bundle adjustment and dense point clouds | Review files | 2026-11-12 | metadata |
| P1 | SfM / geometry | [openMVG/openMVG](https://github.com/openMVG/openMVG) | Multi-view geometry and feature-matching pipeline | MPL-2.0 | 2026-11-07 | metadata |
| P1 | MVS / mesh | [cdcseacave/openMVS](https://github.com/cdcseacave/openMVS) | Dense surface reconstruction, mesh and texturing backend | AGPL-3.0 | 2026-11-16 | metadata |
| P1 | AI reconstruction | [facebookresearch/vggt](https://github.com/facebookresearch/vggt) | Visual Geometry Grounded Transformer for pose/geometry estimates | Review files | 2026-05-19 | metadata |
| P1 | Feature matching | [cvg/Hierarchical-Localization](https://github.com/cvg/Hierarchical-Localization) | Hierarchical localization and robust image matching | Apache-2.0 | 2025-12-10 | metadata |
| P1 | Point cloud / mesh | [isl-org/Open3D](https://github.com/isl-org/Open3D) | Point-cloud processing, registration and geometry operations | Review files | 2026-11-14 | metadata |
| P1 | Geometry kernels | [libigl/libigl](https://github.com/libigl/libigl) | Geometry algorithms incl. parameterization and deformation; metadata/license text differ | License mixed; audit | 2026-10-29 | metadata |
| P1 | Mesh processing | [cnr-isti-vclab/meshlab](https://github.com/cnr-isti-vclab/meshlab) | Filtering, remeshing, scan cleanup and reconstruction tools | GPL-3.0 | 2026-11-02 | metadata |
| P1 | Interactive retopo | [CGCookie/retopoflow](https://github.com/CGCookie/retopoflow) | Interactive retopo workflow over a reference mesh | Review files/assets | 2026-11-09 | metadata |
| P1 | Quad remesh | [ksami/QRemeshify](https://github.com/ksami/QRemeshify) | QuadWild/Bi-MDF based guided quad remeshing | GPL-3.0 (wrapper) | 2024-10-08 | README |
| P1 | UV / packing | [DanielBoxer/UVgami](https://github.com/DanielBoxer/UVgami) | OptCuts/xatlas/PartUV orchestration | GPL-3.0 (wrapper) | 2026-11-15 | metadata |
| P1 | Animation / retarget | [Mwni/blender-animation-retargeting](https://github.com/Mwni/blender-animation-retargeting) | Transfer pose/actions across differing armatures | Review files | 2026-08-09 | README |
| P1 | Rigging / animation | [asu37/Rigger](https://github.com/asu37/Rigger) | Rig creation; smart markers; transfer; mocap workflow | GPL-3.0 | 2026-10-19 | README |
| P1 | Animation / retarget | [MondRubberduck/Bulletproof-Retarget](https://github.com/MondRubberduck/Bulletproof-Retarget) | Rig standard matching, root motion and reversible scene operations | Review files | 2026-11-13 | README |
| P1 | Multi-object pose | [19829984/Blender_Multires_Transpose](https://github.com/19829984/Blender_Multires_Transpose) | Low-resolution merged proxy and iterative multires reshape | MIT | 2023-06-17 | code |
| P2 | Anatomy / muscles | [evaherbst/MyoGenerator](https://github.com/evaherbst/MyoGenerator) | Research muscle volume creation for older Blender 2.91-2.93 | Review files | 2022-12-13 | README |
| P2 | Muscle / soft body | [Himadri-Tech/Blender-Add-ons](https://github.com/Himadri-Tech/Blender-Add-ons) | Muscle template using Hooks, Soft Body and Dynamic Paint | MIT | 2025-12-03 | README |
| P2 | Local retopology | [negdo/LocalRemesher](https://github.com/negdo/LocalRemesher) | Region-local particle/grid remeshing approaches | Review files | 2024-03-28 | README |
| P1 | Procedural geometry | [alessandro-zomparelli/tissue](https://github.com/alessandro-zomparelli/tissue) | Tessellation, reaction diffusion and organic surface generation | Review files | 2026-10-31 | metadata |
| P1 | Nodes / parametric | [nortikin/sverchok](https://github.com/nortikin/sverchok) | Parametric geometry operations and custom node tree techniques | GPL-3.0 | 2026-11-16 | README |
| P1 | CAD / constraints | [hlorus/CAD_Sketcher](https://github.com/hlorus/CAD_Sketcher) | Constraint solver for precision sketches | GPL-3.0 | 2026-11-16 | metadata |
| P1 | Procedural worlds | [princeton-vl/infinigen](https://github.com/princeton-vl/infinigen) | Large procedural scene and synthetic data generation | BSD-3-Clause | 2026-11-11 | metadata |
| P1 | Dataset / automation | [DLR-RM/BlenderProc](https://github.com/DLR-RM/BlenderProc) | Automated scene generation, camera calibration and annotations | GPL-3.0 | 2026-01-20 | metadata |
| P1 | 3DGS / rendering | [Kiri-Innovation/3dgs-render-blender-addon](https://github.com/Kiri-Innovation/3dgs-render-blender-addon) | Gaussian splat render and integration for Blender | GPL-2.0 | 2026-11-11 | metadata |
| P2 | 3DGS / scene edit | [Anttwo/sugar_frosting_blender_addon](https://github.com/Anttwo/sugar_frosting_blender_addon) | Gaussian Frosting reconstructed-scene manipulation | Review files | 2024-09-23 | metadata |
| P2 | NeRF / datasets | [nerfstudio-project/nerfstudio](https://github.com/nerfstudio-project/nerfstudio) | Neural radiance field workflow and dataset processing | Apache-2.0 | 2025-07-29 | metadata |
| P1 | AI texture | [FrederikHasecke/diffused-texture-addon](https://github.com/FrederikHasecke/diffused-texture-addon) | Multiview depth/normal ControlNet texture bake | GPL-3.0 | 2026-10-24 | README |
| P2 | Camera / projection | [PDE26jjk/CamProj](https://github.com/PDE26jjk/CamProj) | Camera storage and projection painting helper | MIT | 2026-01-31 | README |
| P2 | Camera projection | [p4ymak/eyek](https://github.com/p4ymak/eyek) | Rust multiview image-to-UV projection | No licence found | 2024-02-06 | code |
| P2 | Node / baking | [Thomasims/BakeTarget](https://github.com/Thomasims/BakeTarget) | Bake-to-image/vertex-color shader node UX | Unlicense | 2024-09-15 | README |
| P2 | Baking | [projectuniverse/openbake](https://github.com/projectuniverse/openbake) | Baking orchestration addon | GPL-3.0 | 2026-08-12 | metadata |
| P1 | Fluid simulation | [rlguy/Blender-FLIP-Fluids](https://github.com/rlguy/Blender-FLIP-Fluids) | C++ liquid FLIP simulation integrated in Blender | GPL-3.0 | 2026-11-10 | metadata |
| P2 | BIM / CAD | [IfcOpenShell/IfcOpenShell](https://github.com/IfcOpenShell/IfcOpenShell) | Parametric geometry C++ and IFC model | LGPL-3.0 | 2026-11-16 | metadata |
| P1 | Pipeline / export | [EpicGames/BlenderTools](https://github.com/EpicGames/BlenderTools) | Bridge and export automation between Blender and Unreal | MIT | 2026-09-09 | metadata |
| P1 | Agent automation | [hitkeeen/blender-ai-mcp-addon](https://github.com/hitkeeen/blender-ai-mcp-addon) | MCP scene tools, Python scripting and multi-agent workflow | MIT | 2026-11-12 | metadata |
| P2 | Collaborative editing | [niwanotorico/FreebirdCollabo](https://github.com/niwanotorico/FreebirdCollabo) | Realtime remote edits across Blender clients | GPL-2.0 | 2026-10-27 | metadata |
| P2 | 3D differentiable | [facebookresearch/pytorch3d](https://github.com/facebookresearch/pytorch3d) | Differentiable mesh and camera tools for optimization | Review files | 2026-11-13 | metadata |
| P0 | Blender upstream | [blender/blender](https://github.com/blender/blender) | Primary C/C++/Python upstream source and APIs | GPL family; verify files | 2026-11-16 | metadata |
| P2 | Legacy addons mirror | [blender/blender-addons](https://github.com/blender/blender-addons) | Read-only archived GitHub mirror | Review files | 2025-06-17 | metadata |

## Experiments to run first (all pending)

| Priority | Candidate | Target | Discriminating validation / go-no-go |
|---|---|---|---|
| P0 | [CurioCrafter/CurioMesh](https://github.com/CurioCrafter/CurioMesh) | Sculpt / photogrammetry | Compare quad flow, camera silhouette residual and recovery of failed regions |
| P0 | [shaderko/remi-blender-addon](https://github.com/shaderko/remi-blender-addon) | Photogrammetry | Run repair→remesh→UV→bake on intentionally incomplete scan; audit bundled engine licenses |
| P0 | [gokayfem/gaussian-splat-to-blender-nodes](https://github.com/gokayfem/gaussian-splat-to-blender-nodes) | Photogrammetry / shading | Benchmark seam error and color discontinuity across 8-16 calibrated views |
| P0 | [ucupumar/ucupaint](https://github.com/ucupumar/ucupaint) | Polypaint / materials | Compare layer blend UX with existing Multires HR color masters |
| P0 | [jpcy/xatlas](https://github.com/jpcy/xatlas) | Photogrammetry / baking | Compare island distortion, pack efficiency, seam density and speed |
| P0 | [BradyAJohnston/MolecularNodes](https://github.com/BradyAJohnston/MolecularNodes) | Muscle / deform | Prototype stretch/compression constraint with fixed endpoints; measure energy and runtime |
| P1 | [AcademySoftwareFoundation/OpenVDB](https://github.com/AcademySoftwareFoundation/OpenVDB) | Photogrammetry / remesh | Evaluate memory vs dense grids for partial-scan completion and mask-protected regions |
| P0 | [colmap/colmap](https://github.com/colmap/colmap) | Photogrammetry | Compare current addon alignment against COLMAP CLI baseline with same dataset |
| P1 | [openMVG/openMVG](https://github.com/openMVG/openMVG) | Photogrammetry | Compare sparse point coverage and drift on poor overlap |
| P1 | [cdcseacave/openMVS](https://github.com/cdcseacave/openMVS) | Photogrammetry | Isolated integration PoC; legal review of service/distribution obligations |
| P1 | [facebookresearch/vggt](https://github.com/facebookresearch/vggt) | Photogrammetry | Benchmark quality vs COLMAP fallback on same photos; retain optional install |
| P1 | [cvg/Hierarchical-Localization](https://github.com/cvg/Hierarchical-Localization) | Photogrammetry | Measure inliers, registered cameras, pose accuracy vs baseline |
| P1 | [isl-org/Open3D](https://github.com/isl-org/Open3D) | Photogrammetry / facewrap | Test editable cloud filtering and fast local recalc |
| P1 | [libigl/libigl](https://github.com/libigl/libigl) | Geometry / sculpt | Select an individual permissively licensed module after file-level audit |
| P1 | [cnr-isti-vclab/meshlab](https://github.com/cnr-isti-vclab/meshlab) | Photogrammetry | Benchmark repair operations and output fidelity vs Blender native |
| P1 | [CGCookie/retopoflow](https://github.com/CGCookie/retopoflow) | Facewrap / retopo | Compare UX and vertex snapping; audit code vs bundled assets licenses |
| P1 | [ksami/QRemeshify](https://github.com/ksami/QRemeshify) | Retopo | Check engine licenses, old runtime compatibility and group/crease constraints |
| P1 | [DanielBoxer/UVgami](https://github.com/DanielBoxer/UVgami) | UV / photogrammetry | Audit engine downloads/licences; test overlap and density on test assets |
| P1 | [Mwni/blender-animation-retargeting](https://github.com/Mwni/blender-animation-retargeting) | Quick Pose / rigging | Retarget body motion and inspect twist, rest pose and foot sliding |
| P1 | [asu37/Rigger](https://github.com/asu37/Rigger) | Quick Pose / creature | Prototype small quadruped and compare weight quality |
| P1 | [MondRubberduck/Bulletproof-Retarget](https://github.com/MondRubberduck/Bulletproof-Retarget) | Rigging | Study transaction safety, bone mapping and multi-action export |
| P1 | [19829984/Blender_Multires_Transpose](https://github.com/19829984/Blender_Multires_Transpose) | Quick Pose / Multires | Test layer preservation, per-object identity and rollback; see issue #6 |
| P1 | [alessandro-zomparelli/tissue](https://github.com/alessandro-zomparelli/tissue) | NanoMesh / procedural | Compare repeat pattern anchoring and deformation stability |
| P1 | [nortikin/sverchok](https://github.com/nortikin/sverchok) | General Blender / procedural | Prototype repeatable high-level procedural operators |
| P1 | [hlorus/CAD_Sketcher](https://github.com/hlorus/CAD_Sketcher) | Hard surface / modeling | Test deterministic constraint resolution and UX |
| P1 | [princeton-vl/infinigen](https://github.com/princeton-vl/infinigen) | Procedural / rendering | Generate few controlled assets and profile render, geometry size |
| P1 | [DLR-RM/BlenderProc](https://github.com/DLR-RM/BlenderProc) | Photogrammetry testing | Create reproducible synthetic photo datasets with known ground truth |
| P1 | [Kiri-Innovation/3dgs-render-blender-addon](https://github.com/Kiri-Innovation/3dgs-render-blender-addon) | Photogrammetry / render | Compare splat workflow and depth data with point/mesh pipeline |
| P1 | [FrederikHasecke/diffused-texture-addon](https://github.com/FrederikHasecke/diffused-texture-addon) | Texture / relighting | Measure consistency on UV seams and illumination variance |
| P1 | [rlguy/Blender-FLIP-Fluids](https://github.com/rlguy/Blender-FLIP-Fluids) | Simulation | Study solver/render separation and cache/undo behaviour |
| P1 | [EpicGames/BlenderTools](https://github.com/EpicGames/BlenderTools) | Pipeline | Study validation, naming, batch and error recovery |
| P1 | [hitkeeen/blender-ai-mcp-addon](https://github.com/hitkeeen/blender-ai-mcp-addon) | Developer productivity | Safe sandbox test scene commands; avoid arbitrary code on production assets |
| P0 | [blender/blender](https://github.com/blender/blender) | All fork modules | Watch targeted changes to CCG, PBVH, modifiers, Geometry Nodes, renderer |

## Notable source-code anchors already inspected

- [CurioMesh native Rust field meshing](https://github.com/CurioCrafter/CurioMesh/blob/main/native/src/field.rs) and [UV packer](https://github.com/CurioCrafter/CurioMesh/blob/main/native/src/uvpack.rs): oriented field smoothing, multi-policy atlas packing, mip-safe gaps, deterministic kernels. **No compiled/runtime validation here.**
- [Multiview projection core](https://github.com/gokayfem/gaussian-splat-to-blender-nodes/blob/main/skills/gaussian-splat-to-blender-nodes/scripts/projection_core.py): view weighting, depth validation, color blending and fallback. Inspect data/model licences before reusing.
- [MolecularNodes XPBD Solve Points](https://github.com/BradyAJohnston/MolecularNodes/blob/main/molecularnodes/nodes/geometry/_shared/xpbd_solve_points.py): GN point constraints based on inverse mass and distance constraints; this is not proof of a complete muscle simulator.
- [Blender Multires Transpose](https://github.com/19829984/Blender_Multires_Transpose/blob/main/multires_transpose.py) + [proxy helpers](https://github.com/19829984/Blender_Multires_Transpose/blob/main/utils/utils.py): multi-object proxy and Multires reshape; inspect state restoration and layer safety. Already linked to [Sculpt Extended #6](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/6).

## Existing issues / avoid duplicates

- [#6](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/6): Quick Pose, Poseable Symmetry and Multires Transpose comparison.
- [#7](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/7): MicroDetail; sparse layers already exist.
- [#8](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/8): Adaptive Sculpt LOD.
- [#9](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/9): NanoMesh.
- [#10](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/10): ZBrush portability and tools.
- [#14](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/14): mask-adaptive protected remesh.
- [#16](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/16): Multires Layers next generation, Bmax and protected remesh.

## Research / integration decision record

An item graduates from `research` to its own implementation issue **only after**:

1. Precise reusable algorithm or subsystem identified in exact files with valid licences;
2. Comparison with upstream Blender and code already present in the fork;
3. Minimal reproducible experiment and representative data fixture;
4. Runtime + quality assessment, memory, undo/save and regression results;
5. A deployment decision (native C++, bpy addon, external optional worker, GN asset, or reject) and rollback strategy.

**No merges, releases, automatic downloads, or new Blender UI are authorized by this radar alone.**
