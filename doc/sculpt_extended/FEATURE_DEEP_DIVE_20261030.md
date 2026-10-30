# Sculpt Extended — feature deep dive and Blender upstream survey (30 October 2026)

**Scope:** All sculpt, painting, Multires, stroke and workflow features **except Sculpt pivot**, which is addressed separately. This note documents **research and implementation candidates**, not features proven in our build. Upstream Blender 5.3 was in **beta** on this date; 5.4 was in development. The approved Sculpt Extended source is custom Blender 5.2.3 at `233b1a2da926d9200da5321674e54038875813b1`.

## Findings worth adding to the earlier community roadmap

| ID | Feature / technical lead | Evidence / maturity 30 Oct | Priority | Recommended minimum implementation | Main risk & verification |
|---|---|---|---|---|---|
| BR-01 | Independent **Sculpt Texture + Texture Mask** | Discussed at Sept 1 2026 Sculpt/Paint meeting; legacy brush texture pipeline still active | P1 | Independent texture vs mask with separate coordinates/strength, combine existing sculpt geometry mask only at factor stage; active-layer-only application | Preview parity in Sculpt, CCG seam behavior, pressure/falloff, UV/3D coords, frozen stroke mask |
| BR-02 | **Directional Smooth** (stroke/cross direction), optionally curvature guided | Sculpt Dev concept explicitly resurfaced Mar 3 2026 as long-requested | P1 | Extend our layer-isolated delta Smooth with directional tangent-plane kernel, isolated enum; preserve non-active layers | Stability at seams and poles, anisotropic over-smoothing, strength accumulation and deterministic Undo |
| BR-03 | **Reusable detail → VDM Brush** | Community prototype: Mask Extract VDM from Multires; another GN non-Multires method, Mar 6 2026 | P1/P2 | Extract from mask/selected layer as a VDM brush asset; auto derive reference surface, orientation, padding and mapping | Severe curvature, overhangs, UV boundaries; need exact test of vector direction and seam continuity |
| BR-04 | **Rake + Roll + textured strokes** | Official Jerry Wei brush-engine work 2026; rake brush-local instead of texture-local discussed Oct 13; roll mapping PR review | P1/P2 | Separate tip-space orientation from texture-space mapping, add stable path-parallel roll/rake; optional alpha tangent-follow/ribbon | Mirroring/radial symmetry, rotation state at stroke boundary, pressure and discontinuities |
| BR-05 | **Hard-surface Polish / Trim V2** | Julien Kaspar summarized digging, concave flatten, unnatural edges, hard-edge loss Sept 1; WIP plane sampling PR #165216 for 5.4 | P1 | Reference plane sampling backport and prototypes with curvature-aware patch, edge constraints and user-selected plane; layer-only write | Face set/hard-edge preservation, double surfaces, concave mesh and thin parts |
| BR-06 | **Brush engine upstream 5.3** | Tip roundness, rounded cursor, per-brush unified settings, hardness/autosmooth/spacing curves documented as 5.3 upstream; GSoC 2026 reports details | P1 selectively | Cherry-pick low-risk isolated improvements; do not duplicate 5.3 infrastructure by hand | `calc_brush_local_mat` conflicts, brush asset versioning, symmetry, old user brushes |
| BR-07 | **Anchored first dab / pressure** | GSoC 2026 describes initial-pressure locking as design and #110794 first-stroke delay investigation, not all resolved | P1/P2 | Precompute initial stroke pressure and hit-dependent direction when needed; keep normal and anchored modes distinct | First dab changed output, tablet pressure, mouse path, non-deterministic stroke replay |
| BR-08 | **Procedural brush textures via Geometry Nodes** | Blender module discussions and PR #152295 indicate work to replace/extend legacy texture node brush eval; not verified merged | P2 research | Evaluate GN texture source via new interface, expose fields/attributes and masks to brushes; cache when possible | Per-dab field eval costs, asset persistence, coordinate and dependency graph invalidation |
| TP-01 | **3D Texture Paint migration** | Official Sculpt/Paint meetings: migrated Draw into Texture Paint, Mask landed by Oct 13; Blur/Smudge/Clone WIP | P1 reference | Reuse upstream 3D surface paint raycast/coverage, mask & dirty update when implementing Surface-Wrap/stencil grid | Live UVs, occlusion, strokes on thin limbs, hidden geometry and UDIM bounds |
| TP-02 | **Surface Wrap stroke, Grid Stencil** | Our own UX requirement, partly covered by upstream surface-follow Paint and rake/roll exploration | P1/P2 original | Optional surface-follow brush footprint; grid conform to target, orientation stable through curvature; keep ordinary stencil | Contours/seams, discontinuity, overlap, stencil resolution independence |
| TP-03 | **Multi-channel and layered textures** | Blender official Texturing project in development; image multi-layer demo at Blender Conference Oct 2026; official 2026 outlook said full layered-texture release was not scheduled in 2026 | P2 integration | Keep geometry Sculpt Layers distinct from material image layers; share masks/procedural brush sampling; color/roughness/displacement channels in first simple workflow | Don't duplicate evolving official texture stack or lock .blend version format |
| TP-04 | **Paint through / automasking / selection** | Sep 15 meeting: 3D Paint face selection/tiling/scene spacing landed, automasking for thin geometry still under investigation | P1/P2 | Backface occlusion options, visible-only, Face Set/face selection, xray and thickness-aware prevention of bleed | Ears, fingers, seams, multiple close surfaces, stacked UV islands |
| TP-05 | **Blur/Smudge neighbor index** | Oct 13: KDTree searches expensive even for 9 neighbors; hash grid, octree, 2D fallback considered | P2 | Dedicated footprint spatial index, resolution-independent mip-like smoothing; per-tile dirty updates | Grid rebuild per stroke vs per sample, UDIM boundary blends, RAM, high-frequency detail |
| TP-06 | **Wet Mix / Flow settings** | Porting existing Paint brush controls to Vertex/Texture Paint discussed Sept 1; not claimed fully merged | P2 | Flow accumulation + persistent wetness optional for texture paint and HR Polypaint (not a geometry property by default) | Premultiplied alpha, compositing order, overdraw and undo |
| MR-01 | **High-res Face Sets, color, masks** | Upstream 5.3 adds display/interpolation of generic Multires attributes (not an HR Face Set master). Our build has custom HR data infrastructure | P1 | Keep master on highest CCG grids, derive low-level views; copy/transfer with stable corner IDs | CCG stitch/ownership, remesh migration, L0 edit and save/reopen |
| MR-02 | **Multires level-aware displacement / Delete Lower** | Official prototype and incomplete feature discussed Nov 2025 / Apr 2026; upstream bugs around lower-level propagation | P2/P3 architecture | Design conversion from one level into new base preserving layer delta, face sets, color and mask; compare to sparse full-resolution master | High risk of losing geometry; exact roundtrip & rollback required |
| MR-03 | **Mask/hidden/attribute transfer on Apply and Dyntopo** | Feb 2026 developers note Apply Multires does not transfer mask/hidden; Dyntopo undo attribute retention WIP in 2025 | P1 correctness | Explicit data-type migration matrix for Apply/Join/Remesh/Convert/Dyntopo; test before modify | Hidden high-res master accidentally discarded |
| MR-04 | **UV smoothing, actual UV seams** | DevTalk 29 Oct proposal for concave/junction-preserving UV Smooth defaults, actual UV seam overlay | P2 | Preset + overlay + UV overlap/seam detector for displacement & bake; keep original-file defaults | Different tile/UV island coverage and baker output |
| PF-01 | **PBVH CPU/GPU locality and node coverage** | GSoC 2026 found high-poly brush crevices at PBVH node boundaries; official Aug 28 GPU culling 1-draw-call prototype | P1 | Backport verified PBVH node gather fixes, separate benchmark index buffers vs culling vs dirty-region update | Node ownership, OpenGL/Vulkan parity, hidden grids and mask correctness |
| PF-02 | **Memory-efficient local Sculpt Layers + cache** | Our approved code keeps `LayerPayloadIndex` single-entry, and current Smooth per-stroke prepared grid data | P1/P2 | Per-layer cache versions, completed-grid state, sparse displacement blocks with separate experimental branch | Invalidated pointer access, serialization/Undo, total memory peak vs synthetic ratio |
| PF-03 | **Save/autosave responsiveness** | Direct Oct 22 2026 forum tester reported severe freezes when autosaving ~80M-face scene; our checkpoint does not claim resolution | P1 | Profile serialization, memory peak and interruption; incremental/async only after consistency design | Atomic file state, crash-safe recovery, stale grids, transaction barriers |
| UX-01 | **Brush assets/alpha library** | Community wants bulk alpha browsing and less brush duplication; 5.3 per-brush unified settings | P2 | Browse alpha textures independently from brush assets, optional per-project overrides | Asset lib source changes, versioning and packing |
| UX-02 | **Snapshot/Restore Brush** | User feature demand in forum & RCS, 2026 | P2 | Capture reference geometry or selected Sculpt Layer state; paint restore as isolated delta | Reference resampling level, memory limit, saved identity |
| UX-03 | **Extract geometry from Multires** | Forum Mar 2026 requests Extract from Face Set/mask without Apply Multires | P2 | Generate new independent mesh from evaluated subset, optional shell, non-destructive original | Boundary closure, duplicated seam samples, thick shells and normals |
| UX-04 | **UV grid/stretch overlay & diagnostics** | Aug/Sep 2026 official meeting ideas | P2 | Display map distortion/actual island boundaries in sculpt and paint | Visual performance on high-res grids |

## Integration batches to test (not promises)

**Batch A — low-risk upstream verification / strong short-term value:** audit/bring in PBVH high-poly node-boundary selection fix, first-stroke latency fix if stable, brush pressure curves, 5.3 Multires color/texture drawing support; keep each flaggable and measurable independently. Separate improvements in geometry/performance/UX.

**Batch B — brush-native features using existing Sculpt Layers:** Directional Smooth, Polishing V2, texture+mask factor computation, rake/roll tangent-frame stabilizer, custom tip alpha. No implementation should modify nonactive layer payload.

**Batch C — cross-editor painting:** upstream 3D Texture Paint 5.4 groundwork, optional conforming stencil grid, face-selection/automasking, image-based blur/smudge with spatial sampling and auto-UV checks. First goal is stroke parity and correct selection on closed/near-touching surfaces.

**Batch D — long research projects:** VDM extraction from active layer, dynamic texture-displacement coordinate advection, high-resolution Face Set master interoperability, sparse Multires layers, hierarchical displacement and promote-level-to-base.

## Source/code anchors and status notes

Official docs:
- Blender 5.3 Sculpt release notes: https://developer.blender.org/docs/release_notes/5.3/sculpt/
- Blender 5.2 Sculpt release notes (Scene Project brush and remesh attribute interpolation): https://developer.blender.org/docs/release_notes/5.2/sculpt/
- Blender 2026 roadmap, notes that finished layered-texture editing was *not* expected to ship in 2026: https://www.blender.org/development/projects-to-look-forward-to-in-2026/
- Blender roadmap Texturing project: https://www.blender.org/roadmap/

Upstream meetings and GSoC:
- 2026-09-01: https://devtalk.blender.org/t/2026-09-01-sculpt-paint-texture-module-meeting/45793
- 2026-09-15: https://devtalk.blender.org/t/2026-09-15-sculpt-paint-texture-module-meeting/45816
- 2026-10-13: https://devtalk.blender.org/t/2026-10-13-sculpt-paint-texture-module-meeting/46025
- 2026-10-23 Project Updates: https://devtalk.blender.org/t/2026-10-23-project-updates-meeting/46135
- 2026-03-03: https://devtalk.blender.org/t/2026-03-03-sculpt-paint-texture-module-meeting/44512
- 2026-02-17 Texture Paint/Bake kickoff: https://devtalk.blender.org/t/2026-02-17-texture-paint-baking-kickoff-meeting/44459
- 2026-04-28: https://devtalk.blender.org/t/2026-04-28-sculpt-paint-texture-module-meeting/45209
- 2026-08-28 GPU/PBVH prototype: https://devtalk.blender.org/t/2026-08-28-project-updates-meeting/45739
- GSoC Brush Engine Aug 2026 final report and history: https://devtalk.blender.org/t/gsoc-2026-general-brush-engine-improvements/45188
- GN brush texture discussion: https://devtalk.blender.org/t/2026-01-06-sculpt-paint-texture-module-meeting/43956
- Texturing stack Oct 9: https://devtalk.blender.org/t/2026-10-09-project-updates-meeting/45951

Community:
- Mask-based VDM extraction with 2 working prototypes: https://devtalk.blender.org/t/extract-vdm-brush-from-sculpt-proposal/44505
- Sculpt Extended tester and problem reports: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=274
- Sculpt Rake/Roll and Sculpt Layer demand: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=266
- Multires extraction request: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=261

Relevant existing source files (approved reference) to review before writing new code:
- `source/blender/editors/sculpt_paint/mesh/brushes/smooth.cc`
- `source/blender/editors/sculpt_paint/mesh/sculpt_layer_delta_smooth_v01.hh`
- `source/blender/blenkernel/intern/multires_sculpt_layers.cc`
- `source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc`
- `source/blender/editors/sculpt_paint/mesh/brushes/draw.cc`

## Non-negotiable test rules

1. Preserve active Sculpt Layer-only deformation, other layer data/mask/color/HR Face Set identities, full L0 and levels 1..max, and Edit Mode remapping.
2. Verify cross-level CCG seam samples, hidden regions and near-touching parts in all new brush modes.
3. Test saved scenes and Undo/Redo, multiple active/hidden/negative layers and strength animation; do not silently alter file format or defaults.
4. Record exact Git SHA, flags, Blender release base, hardware/GPU backend, scene polycount, cold/warm timings, RAM/VRAM peaks and geometric comparison.
5. Label documentation as **upstream merged**, **in review**, **proposal**, **our existing**, or **new experiment**. Never infer a PR was merged solely from roadmap discussion.
