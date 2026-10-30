# Community requests and improvement roadmap — 30 October 2026

Research note for Blender Sculpt Extended 5.2.3. This is **not** an implementation claim or a new validated checkpoint. Sources are community requests, discussions, and testimony; avoid representing individual reports as statistically representative demand. Right-Click Select is a read-only archive as of October 2026.

## Existing code audit to retain (28–30 Oct)

- **P0 correctness:** mixed L0→L1 Undo level restoration, unsaved L0 RNA edits before first stroke, intermediate-level Draw Preserve guard and Draw Transport edge cases. See `STABLE_CHECKPOINT_20261027.md`.
- **P1 Smooth Cache V2:** track completed-grid validity via generation/dirty state rather than re-scanning `BitVector` for `complete_grid_data()` in a hot path; profile neighborhood lookup `BKE_subdiv_ccg_neighbor_coords_get()`; consider topology-scoped neighbors cache and safe across-stroke reference reuse. Must not keep invalid references after CCG rebuild or change numerical accumulation order.
- **P1 Layer Payload Cache V2:** current `LayerPayloadIndex` in `source/blender/blenkernel/intern/multires_sculpt_layers.cc` holds only one cached layer index; experiment with per-layer per-generation support indices, explicit mutation invalidation, bounds/ownership checks, and cache size limits. Benchmark alternating layers and strength/visibility switches.
- **P1 PBVH/viewport:** compare sharing of OpenGL immutable compatible triangle indices with PBVH draw-call reduction and dirty-region update paths. Separate actual GPU/VRAM savings from counter fixes.
- **P1 sparse Sculpt Layers experiment:** zero/unallocated regions should not require full displacement payload. Preserve save/load, subdivision, Undo, seam identity, masked, hidden and L0 behavior. Develop on a separate branch; never assume backwards-compatible file serialization.
- **P1 diagnostics:** benchmark brush preparation, calculation, commit, undo compression, viewport upload and save/autosave as distinct phases. Report CPU/RAM/VRAM and exact file+commit/build flags.

Code references: 
- `source/blender/editors/sculpt_paint/mesh/brushes/smooth.cc`;
- `source/blender/editors/sculpt_paint/mesh/sculpt_layer_delta_smooth_v01.hh`;
- `source/blender/blenkernel/intern/multires_sculpt_layers.cc`;
- `source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc`.
Approved optimized source: `233b1a2da926d9200da5321674e54038875813b1`.

## Direct feedback on Sculpt Extended (Blender Artists, 21–23 October 2026)

Source: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=274

1. **Dense scene saving and autosave:** one user reports acceptable experience on a 40-million-triangle head (Ryzen 7 1700/32 GB RAM); 80 million would no longer be comfortable and autosave causes significant freezes. Treat as one real-world report, not a benchmark or guaranteed capability. Action: measure blocking save/snapshot time and memory spikes; inspect feasibility of incremental serialization and/or scheduling **without** endangering file integrity.
2. **Hiding geometry does not necessarily accelerate brush strokes:** another user finds it helps little beyond display. Action: inspect PBVH node mask, hidden-grid skips, brush factors and evaluated sample traversal; benchmark visible vs hidden with equal stroke paths.
3. **Getting started:** a user asked how to run the portable ZIP. Action: one-page quick start, launcher names, screenshots/text for Windows and Linux; distinguish approved feature switches from launching `blender.exe` directly.
4. **Linux demand:** direct request for Linux package; publish exact source SHA and feature-parity status, do not conflate different Windows/Linux source builds.
5. **Trust and maintainability:** discussion around small forks and abandonment risk. Action: publish source, provenance, changelog, known limitations, executable hashes, reproducer scripts and a compatibility roadmap.
6. **Positive test:** one real-world 40-million-triangle head working well is encouraging, but does not prove every brush/layer/driver combination.

## Other user-requested features (priority = recommendation, NOT popularity vote)

### A. Strong workflow fit / high project relevance

| ID | Proposal | User need evidenced by discussion | Our candidate approach | Proposed priority |
|---|---|---|---|---|
| F01 | **Morph / Restore / Snapshot Brush** | Revert a local region to an earlier sculpt state, not erase all multires displacement; morph target blending. | Restore against selected saved snapshot, base, previous layer state or reference. Preserve protected layers and masks. | P1/P2 |
| F02 | **Brush alpha roll/rake / surface-follow** | Alphas for seams, stitches, wrinkles should orient/deform along curved strokes rather than repeat identical stamps; improved asset browsing. | Extend stroke/brush engine to rotate/tile alpha along tangent, optional path deformation and seam-aware projection; consistent Sculpt and Texture Paint UI. | P1/P2 |
| F03 | **Quick Pose/Transpose + Posed Symmetry** | Pose Brush control/pivot limitations; users need symmetry after character pose and a manipulator without mode switching. | Native, Sculpt Mode-available, optional temporary deformation and topology correspondence, with layer-aware transactions. | P2 |
| F04 | **Face Set safe Voxel Remesh** | Touching volumes with distinct Face Sets (e.g. fingers) merge despite Preserve Face Sets. | Option to voxelize each Face Set as a separate material/region then stitch only requested contacts; keep boundaries/labels. | P2 |
| F05 | **Extract from Multires mask/Face Sets** | Users want extracting surfaces/parts in Sculpt Mode without applying Multires and rebuilding subdivision. | Generate independent mesh from evaluated selected region, with boundary cleanup; non-destructive original. | P2 |
| F06 | **High-resolution Face Sets / color / mask attribute preservation** | Artist concern over face sets/polypaint at Multires resolution, preserving information across levels or Dyntopo/remesh. | Master at highest CCG level with derived lower-level display; corner ID remap; no lossy lower-level overwrite. | P1/P2 |
| F07 | **Texture/noise mesh filter with mask** | NoiseMaker-like procedural surface detail that can be locally painted away. | Procedural displacement directly to active Sculpt Layer using mask and coordinates; later brush/filter modulation. | P2 |
| F08 | **Directional/peak/valley Smooth** | Different Smooth variants, smoothing crests vs valleys and brush-direction-constrained smoothing. | Reuse layer-isolated reference and directional kernel; protect nonactive layers. | P2 |

### B. Useful smaller workflow requests

- **Per-project temporary brush settings:** users disagree over when brush edits should reset, but want brush size/strength/alpha changes to survive project reopen without endlessly duplicating assets. Design explicit “per-project overrides” vs “saved asset” preference.
- **Alpha texture library UX:** folders of alpha images should be browseable/importable without requiring a new brush asset for each image.
- **Adjustable sculpt pivot:** surface-aligned visual pivot, quick placement and numeric repositioning; part of Quick Pose/Transpose.
- **Per-object sculpt history:** raised in user discussions; research cost and implications for Blender's global Undo before considering.
- **Annotation→stroke:** resampled annotation paths converted to genuine sculpt strokes with tapered pressure. An Oct 2026 community addon shows concrete demand and potential API reuse.
- **Texture Paint parity:** recent official module meetings discuss Draw, Blur/Smooth, masking, symmetry and brush performance. Avoid rebuilding mature code.
- **Quad remeshing with thin-feature preservation and guides:** users want usable quad retopology from sculpt without paid add-ons; aligns with the separate QuadriFlow+ initiative.
- **Editable subdivision base / delete lowest Multires level:** a request for promoting a higher level as new base; dangerous with existing displacement/layers and must be treated as a major remapping operation.

## Evidence / source URLs (all evaluated by 30 Oct 2026)

Direct build feedback:
- https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=274

Users and requests:
- Morph target / erase behavior, September 2026: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=272
- Restore brush proposal (30 Sept 2026): https://blender.community/c/rightclickselect/6Gkz/
- Snapshots and Multires extraction, March 2026: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=261
- Alpha movement request: https://blender.community/c/rightclickselect/wRAJ/
- Alpha asset library workflow: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=255
- Pose brush/pivot critiques: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=256
- Sculpt pose expansion idea: https://blender.community/c/rightclickselect/JQ3g/
- Voxel remesh Face Sets (13 Feb 2026): https://blender.community/c/rightclickselect/XnOl/
- Directional smooth, pose symmetry, >20M triangles, open quad remesher (individual user post, July 2026): https://www.reddit.com/r/blender/comments/1uqn1c8/quality_of_life_features_i_wish_to_see_in_blender/
- Per-project brush settings discussion: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=190
- Roll/rake request: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=266
- Multires low-level promotion: https://blender.community/c/rightclickselect/lvJ7/
- Recent Oct 2026 brush tip roundness discussion: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=273
- Additional Oct 2026 annotation-to-sculpt-stroke addon: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=274
- Multires Polypaint and layers user interest: https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=266
- Official texture, sculpt progress (not same as user requests):
  - https://devtalk.blender.org/t/2026-08-04-sculpt-paint-texture-module-meeting/45660
  - https://devtalk.blender.org/t/2026-10-13-sculpt-paint-texture-module-meeting/46025

## Implementation discipline

- All new features must be clearly labeled **existing**, **partially present**, **planned** or **experimental** after source verification. A forum wish is not a native Blender deficiency without checking current version.
- Respect active layer-only edits; masked/hidden faces, L0, levels L1–max, seam duplicates, original/derived CCG identity, baking and persistent file data.
- Compare OFF/ON reproducibly (hashes/tolerance depending on path); test Undo/Redo and save/reload, including partial failure and cancellation; never silently drop protected data.
- Measure cold/warm strokes at ~5M, ~30–40M and ~120M samples when hardware allows; record time per stage and memory peaks. Any 'faster' claim needs benchmark evidence.
- Preserve approved Windows source checkpoint and distinguish Linux build provenance.


---

## Blender development audit — official upstream, 30 October 2026

**Baseline:** Sculpt Extended approved source is Blender 5.2.3 with custom changes. Blender 5.3 is in beta (expected release 10 Nov 2026), 5.4 is in active development. The entries below are **upstream findings or proposals, not integrated Sculpt Extended features** unless an explicit comparison/cherry-pick and build verification say otherwise. Never import patch ranges without checking our custom sculpt layers, undo and CCG changes.

### 5.2 features already upstream: compare, don't reinvent

- **Scene Project brush**: Blender 5.2 officially includes projecting sculpt vertices to other scene objects, like a local shrinkwrap brush. Compare with our custom layer-aware Shrinkwrap → active Sculpt Layer workflow. Upstream reference: https://developer.blender.org/docs/release_notes/5.2/sculpt/
- **Add Primitive in Sculpt Mode**: official 5.2 already adds primitive operators; research unification with Edit Mode primitives (WIP PR #164094) only if it benefits current sculpt workflow.
- **Voxel Remesh vertex/corner attributes**: 5.2 improved interpolation of color attributes when remeshing. Our HR Face Set/Polypaint master still requires explicit coverage and separate cross-resolution tests.

### Blender 5.3 beta — features documented as integrated upstream

| Feature | Evidence | Sculpt Extended action |
|---|---|---|
| **Multires generic attribute display and textures** | 5.3 release notes: generic mesh/color attributes interpolated and textures display in Sculpt+Multires | P1: compare data domains and render paths with HR Polypaint/color layers, preserving separate CCG master; avoid duplicating upstream rendering fixes. |
| **Transform Only Pivot** | 5.3 release notes | P2: reference UI/controls for Quick Pose / Posed Symmetry; does NOT itself solve posed topology symmetry. |
| **Pressure curves: Hardness, Auto-Smooth, Spacing** | 5.3 release notes and GSoC 2026 general brush engine | P2: review compatibility of stroke interpolation with native layer-only brushes, avoid duplicate controls. |
| **Per-brush unified scene settings opt-in** | 5.3 release notes | P2: useful for brush asset UX and project overrides; respect asset persistence. |
| **Tip roundness / rounded box cursor** | 5.3 notes and Oct 13 meeting | P2: improve hard-surface tip shape; report stair-stepping on sharp corners as quality concern before copying. |
| **Texture Paint image/UDIM undo and efficient mipmaps** | 5.3 release notes | P1: upstream foundation for Texture Paint Surface Wrap; distinguish image Undo from mesh/layer Undo. |
| **Vertex and Weight Paint automasking** | 5.3 notes | P2: survey shared mask/falloff computations to reuse in color/texture painting as appropriate. |

Source: https://developer.blender.org/docs/release_notes/5.3/sculpt/ and https://devtalk.blender.org/t/gsoc-2026-general-brush-engine-improvements/45188

### Current October 2026 commits / in-review / experiments

| Item | Upstream status / evidence | Proposed Sculpt Extended work |
|---|---|---|
| **Optimize calc_vertex_displacement** | Official weekly updates 26 Oct (Nicola Dessi), reported merged | P1: backport feasibility, benchmark Draw/Clay/Plane with active-layer-only policy |
| **Simplify plane sampling / avoid unnecessary calc_brush_plane** | 26 Oct weekly; separate PR #165216 had been WIP in 13 Oct meeting | P1: benchmark and reference for HPolish/Trim; don't claim entire experimental PR has been merged |
| **Reduce Sculpt Buffers** | Jeroen Bakker PR #162144 in Aug review, shown in October weekly changes | P1: compare GPU buffer memory/dirty-node updates with our shared Multires index buffer |
| **PBVH GPU-culling / Multi Draw Indirect** | 28 Aug project meeting says multi-phase development, single-draw-call prototype WIP | P1: investigate compatible viewport path on experimental branch, not automatic port to OpenGL/Vulkan |
| **Brush rake promoted to brush-local control** | October module meeting, work by Jerry Wei | P2: coordinate with Surface Wrap alpha / stencil / tip transforms, avoid double rotation |
| **Roll curve mapping** | March and September module meetings; community PR under review with performance questions | P2: use as reference for seam/stitch/wrinkle strokes, validate corner cases |
| **Texture Paint Blur/Smooth spatial structure** | October module meeting: KDTree 9-neighbor query costly, investigating brush-radius hash grid, octree, 2D fallback | P1: reuse lessons for surface-follow paint and sparse buffer access; avoid blindly porting KDTree |
| **3D Texture Paint Mask brush** | Oct 13 module meeting: landed | P2: integration candidate for Texture Paint brush system |
| **Shape-preserving / Polish brush limitations** | Sept 1 module meeting reports digging into surface, artificial side edges, flattening concavities and inability to preserve hard edges | P1/P2: HPolish V2 with curvature-aware plane, tangent-space masks, stable hard-edge constraints |
| **Multires reshape coordinate traversal** | Hans Goudey official commits around Oct 28: no foreach callbacks | P1: investigate local commit/reshape speed and compatibility with layer persistence |
| **Smooth Taubin/HC/Frequency-Aware** | GSoC 2026 Smooth Modifier research and PRs; not to be described as all present in Sculpt Smooth | P2: Taubin-style layer-isolated Smooth experiment; frequency-aware likely offline/global mesh filter |
| **Delete Lower Multires** | 28 Apr module meeting says a worthwhile missing feature requiring nontrivial displacement rebuilding | P2/P3: potentially Promote Level as Base with full layer/Mask/Polypaint remap and reversibility test |
| **Multires per-level displacement prototype and spike fix** | Feb/Mar meetings; narrow spike patch shelved in March | P1: compare hierarchical per-level idea to our full-resolution layer masters and sparse-grid plan; no automatic mixing of representations |
| **Sculpt on Mirror modifier virtual side** | Aug 4 meeting, idea to route raycasts and mirrored strokes to real vertices | P2: could complement Posed Symmetry but not equivalent; needs topology mapping and layer correctness |
| **Applying Multires preserves mask/hidden** | Feb 3 module meeting notes known missing attribute transfer | P1/P2: apply/bake/export roundtrip preserving mask, Face Sets, color, other layers |
| **UV Smooth settings for Multires** | New 29 Oct proposal: Keep Boundaries may create overlaps; suggest Keep Corners, Junctions and Concave as more sensible default, study seam-aware future method | P1: add non-destructive warning/preset and validation for UV bake & UDIM. Do **not** change existing-file defaults silently |
| **Actual UV seam overlay** | Idea posted 29 Oct: show true UV island boundaries, not only manually marked edges | P2: valuable for detecting seams in multi-res texture/displacement and surface wrap |
| **Sculpt-side UV grid/stretch visualization** | Aug/Sept Sculpt meetings, exploratory UX idea | P2: optional UV stretch/overlap overlay while painting without leaving Sculpt |
| **Transform data allocation reduction** | Oct 26 Modeling meeting reports huge benefit in one isolated case (2 TB → 600 MB, PR #165506), unrelated scope | Research-only: audit only allocation pattern; never transfer the reported benchmark to Multires/our build |
| **Sculpt on high-density scene quality / autosave** | direct user feedback in earlier roadmap; official Texture Paint has saved image undo/cache fixes | P1: maintain separate memory and autosave profiling, don't confuse paint image caching with .blend CCG serialization |

### Important external references

- Upstream 5.3 release notes: https://developer.blender.org/docs/release_notes/5.3/sculpt/
- Upstream 5.2 release notes: https://developer.blender.org/docs/release_notes/5.2/sculpt/
- Oct 13 Sculpt/Paint meeting: https://devtalk.blender.org/t/2026-10-13-sculpt-paint-texture-module-meeting/46025
- Sept 1 Sculpt/Paint meeting: https://devtalk.blender.org/t/2026-09-01-sculpt-paint-texture-module-meeting/45793
- Aug 4 Sculpt/Paint meeting: https://devtalk.blender.org/t/2026-08-04-sculpt-paint-texture-module-meeting/45660
- Mar 3 Sculpt/Paint meeting: https://devtalk.blender.org/t/2026-03-03-sculpt-paint-texture-module-meeting/44512
- Feb 3 Sculpt/Paint meeting: https://devtalk.blender.org/t/2026-02-03-sculpt-paint-texture-module-meeting/44257
- Apr 28 Sculpt/Paint meeting: https://devtalk.blender.org/t/2026-04-28-sculpt-paint-texture-module-meeting/45209
- 28 Aug PBVH project discussion: https://devtalk.blender.org/t/2026-08-28-project-updates-meeting/45739
- 26 Oct weekly changes: https://devtalk.blender.org/t/26-october-2026/46071
- 28 Oct Blender Today sculpt and draw commit roundup: https://devtalk.blender.org/t/touch-your-blender-blender-today-live-292/46170
- New UV smoothing idea (29 Oct): https://devtalk.blender.org/t/uv-better-default-uv-smoothing-on-subdivision-multires-modifiers/46187
- New true UV seam overlay proposal (29 Oct): https://devtalk.blender.org/t/uv-actual-uv-seam-overlay/46191
- Brush engine GSoC 2026: https://devtalk.blender.org/t/gsoc-2026-general-brush-engine-improvements/45188
- Smooth methods 2026: https://devtalk.blender.org/t/feedback-wanted-new-hc-taubin-frequency-aware-smoothing-in-the-smooth-modifier/45726
- Official developer architecture notes (Mesh/BMesh/CCG): https://developer.blender.org/docs/features/sculpt_paint/mesh_paint/

### Short implementation order proposal

1. **Review and measure** Oct 2026 upstream C++ low-risk brush calculations (`calc_vertex_displacement`, plane sampling, Multires reshape traversal) against approved checkpoint; source code inspection and tests required before import.
2. **Compare** upstream attribute display and GPU sculpt buffer reductions to our HR polypaint/viewport branch; possible improvements without adding user-visible complexity.
3. **Prototype** HPolish V2, Taubin-style delta smooth and rake/roll surface-follow on isolated branches.
4. **Explore** UV Smooth presets and actual-seam overlay with tests for saved files, UDIM and bake.
5. **Architecture study only:** per-level Multires displacement, Delete Lower, one-draw-call PBVH, cross-stroke persistent cache and fully sparse layer payload.

All candidate upstream PRs/commits must be checked for exact integrated status at cherry-pick time, compiled on Windows and Linux separately, and tested with 3+ layer arrangements, L0, hidden grids, masks, UVs, high-resolution face sets, Undo/Redo and save/reload.
