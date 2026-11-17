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

## Catalog (69 distinct verified repository endpoints)

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

| P1 | CAD / retopo | [gregcarreno3d-dev/quadforge](https://github.com/gregcarreno3d-dev/quadforge) | Rust 4-RoSy cross-field solver and guide-aware quad remesh; engine-rs/src/stages/field.rs | GPL-3.0 | 2026-11-15 | code |
| P1 | Camera tracking / PnP | [theartful/polychase](https://github.com/theartful/polychase) | C++ robust PnP with LM refinement and camera intrinsics; cpp/pnp/solvers.cc | GPL-3.0 | 2026-05-24 | code |
| P2 | Garments / physics | [MarcelloMorettoni/opensew-2](https://github.com/MarcelloMorettoni/opensew-2) | Pattern/cloth draping; collision collection per garment layer avoids dependency cycles | GPL-3.0 | 2026-08-26 | code |
| P1 | Agent automation | [dcc-mcp/dcc-mcp-blender](https://github.com/dcc-mcp/dcc-mcp-blender) | Typed Blender MCP tools, modular scene/mesh/material/rig operators and integration tests | MIT | 2026-11-15 | metadata |
| P1 | GPU simulation | [NVIDIA/warp](https://github.com/NVIDIA/warp) | Device kernels for springs/integration; asv/benchmarks/examples/cloth.py; FEM extensions | Apache-2.0 | 2026-11-17 | code |
| P2 | 3D deep learning | [NVIDIAGameWorks/kaolin](https://github.com/NVIDIAGameWorks/kaolin) | Differentiable 3D representations, custom CUDA/PyTorch geometry operators | Apache-2.0; models/dependencies separately | 2026-11-16 | metadata |
| P1 | Mesh optimization / LOD | [zeux/meshoptimizer](https://github.com/zeux/meshoptimizer) | Efficient vertex-cache/index compression and mesh LOD clustering | MIT | 2026-10-14 | metadata |
| P2 | Subdivision C++ | [PixarAnimationStudios/OpenSubdiv](https://github.com/PixarAnimationStudios/OpenSubdiv) | Subdivision evaluation kernels, stencils, GPU backends; Blender upstream already uses this | Review files | 2026-10-26 | metadata |
| P2 | Robust geometry kernels | [CGAL/cgal](https://github.com/CGAL/cgal) | Exact predicates, robust mesh repair/intersections and surface/volume meshing | Package-specific GPL/LGPL/commercial audit | 2026-11-14 | metadata |
| P2 | Hair interoperability | [vickyat24fps/Unreal-Groom-Importer](https://github.com/vickyat24fps/Unreal-Groom-Importer) | Unreal Groom OBJ+metadata -> native Blender Hair Curves, root UV and attributes | Review files; extension listing says GPL-3+ | 2026-08-03 | README |
| P2 | Texture / rectification | [gokayago/UVRectifier](https://github.com/gokayago/UVRectifier) | Perspective rectification of trapezoidal photo textures | GPL-3.0 | 2025-12-14 | metadata |
| P2 | AI image-to-geometry | [microsoft/MoGe](https://github.com/microsoft/MoGe) | Single-image metric geometry/depth estimation; research approach | Review files/models (GitHub NOASSERTION) | 2026-11-14 | metadata |
| P2 | 3D Gaussian Splatting | [graphdeco-inria/gaussian-splatting](https://github.com/graphdeco-inria/gaussian-splatting) | Reference 3D Gaussian Splatting training/rendering | Research/model terms; review files | 2025-10-17 | metadata |
| P2 | Point-cloud meshing | [ArthurReboulSalze/Frost-for-Blender](https://github.com/ArthurReboulSalze/Frost-for-Blender) | Particles and points to polygonal surfaces, C++ addon | No recognized license, reuse blocked | 2026-07-14 | metadata |
| P1 | Grease Pencil / 2D→3D | [chsh2/nijiGPen](https://github.com/chsh2/nijiGPen) | 2D Boolean/offset, stroke cleanup and shape-to-mesh generation; operators/operator_mesh.py | GPL-3.0; third-party dependencies separately | 2026-11-07 | README |
| P2 | 2D pipeline / interop | [Malehaly/StoryPaint](https://github.com/Malehaly/StoryPaint) | Bidirectional Blender Grease Pencil ↔ Krita via local bridge, preserve layer/frame identity | GPL-3.0 | 2026-11-12 | README |
| P1 | Rendering / compositing | [EatTheFuture/compify](https://github.com/EatTheFuture/compify) | 3D-space compositing, node workflows, Blender Python API workarounds | Code GPL-2.0 per README; API reports NOASSERTION | 2023-11-22 | README |
| P1 | Scan alignment / ICP | [patmo141/object_alignment](https://github.com/patmo141/object_alignment) | Picked points and iterative closest point object registration; operators/icp_align.py | GPL-3.0 | 2022-09-03 | README |
| P2 | Mesh algorithms / brushes | [Jorgeromeu/blender-geometry-processing](https://github.com/Jorgeromeu/blender-geometry-processing) | ICP, Laplacian smooth, gradient deformation and constraint-based edits | No recognized LICENSE; audit | 2023-12-01 | README |
| P1 | Point cloud / Geometry Nodes | [luigipacheco/mixtli](https://github.com/luigipacheco/mixtli) | Composable GN point-cloud selection, corridor/distance/plane/volume queries, attribute filters | GPL-3.0 | 2026-11-05 | README |
| P2 | Grease Pencil / modeling | [tonis2/GreaseMesh](https://github.com/tonis2/GreaseMesh) | Generate 3D meshes from Grease Pencil strokes | Extension reports GPL-3.0+; repo metadata unrecognized | 2026-10-22 | README |
| P1 | Compositor / render passes | [kunemann/blender-extensions](https://github.com/kunemann/blender-extensions) | LayerCake editable EXR/Cycles/EEVEE pass reconstruction; .build/layercake/{builder,passes}.py | GPL-3.0 | 2026-11-12 | README |
| P2 | Animation / UI | [Pullusb/greasepencil_tools](https://github.com/Pullusb/greasepencil_tools) | Grease Pencil animation enhancements, timeline scrub and keyboard UX | GPL-3.0 | 2026-10-24 | README |
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

## Research wave 2 — additional targeted source audits (2026-11-17)

- [QuadForge cross-field solver](https://github.com/gregcarreno3d-dev/quadforge/blob/main/engine-rs/src/stages/field.rs): per-face four-direction field, feature edges and path guides; **early-stage** and not a drop-in QuadriFlow replacement.
- [Polychase PnP](https://github.com/theartful/polychase/blob/master/cpp/pnp/solvers.cc): C++ iterative weighted PnP with robust loss, Levenberg–Marquardt and inlier ratio; geometry tracking vs dense reconstruction are separate tasks.
- [OpenSew cloth](https://github.com/MarcelloMorettoni/opensew-2/blob/main/clothing_design/sim.py): per-garment collider collections for layered garments; no claim of native muscle/tissue support.
- [Warp cloth kernel](https://github.com/NVIDIA/warp/blob/main/asv/benchmarks/examples/cloth.py): spring evaluation and GPU kernel timing; inspect numerics and CUDA/CPU support for actual deployment.
- [DCC MCP Blender](https://github.com/dcc-mcp/dcc-mcp-blender): typed agent interface; benchmark with sandbox data only and review exposed execution interfaces.

**Added 14 non-duplicate upstream repositories this wave, verified GitHub metadata and selected source excerpts. No build or runtime tests performed.**

## Research wave 3 — compositing, Grease Pencil and point-cloud editing (2026-11-17)

New source anchors:

- [LayerCake](https://github.com/kunemann/blender-extensions/tree/main/.build/layercake) — multi-pass compositing and EXR pass building; test interaction with existing compositor instead of replacing it.
- [Mixtli pointops](https://github.com/luigipacheco/mixtli/blob/main/pointops.py) — GN point-cloud selections and spatial queries, especially interesting for non-destructive exclude/mask in sparse/dense photogrammetry.
- [Object Alignment ICP](https://github.com/patmo141/object_alignment/blob/master/operators/icp_align.py) — older alignment reference for Facewrap; does not solve non-rigid correspondence on its own.
- [NijiGPen mesh operations](https://github.com/chsh2/nijiGPen/blob/main/operators/operator_mesh.py) — 2D vector/stroke-to-geometry.
- [StoryPaint local bridge](https://github.com/Malehaly/StoryPaint/tree/main/blender_krita_bridge/blender) — cross-app transfer of layer/frame metadata.

**All additions are research only: metadata, READMEs and repository trees, not executable validation.**
