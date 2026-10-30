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
