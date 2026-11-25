# Sculpt Extended 5.4 — migration start / 2026-11-25

Status: PREPARATION ONLY. No 5.4 source port, compilation or runtime validation performed yet. This source-inventory branch is intentionally still Blender 5.2.3. Do not merge it into main. Supersedes the *scheduling* of the older handoff, not its performance/data requirements.

## Immutable inputs

| Input | SHA | Role |
|---|---|---|
| Release 2026-11-04 | 09a16d4b8482200101cb30728881ca4524edf019 | Performance/data baseline |
| Physics V30B3 | 6aac2da6b01481fd71b83132882e6ee4e1193bfc | Latest observed development source, experimental |
| Fork main | cd1ecee302c1ef85113a524fab46802d88bfe372 | Audit exclusive fixes |
| Old migration plan | e92c1207f4a3dfd49e1b397915401acff05e0923 | 5.2.1, documentation |
| Old release migration plan | 03f1ea00dab3d4926d1477d8183491906418083f | 5.2.3, documentation |
| Official Blender upstream | 668b05cf77f45b0b599d9aab730f30efd28e7705 | 5.4.0 alpha, verified BKE_blender_version.h at exact SHA |

Upstream: https://github.com/blender/blender/commit/668b05cf77f45b0b599d9aab730f30efd28e7705

GitHub rejected direct creation of migration/54-upstream-20261125 with 422 Object does not exist: the upstream object must first be fetched and pushed from a real Git checkout. Do not substitute a renamed 5.2.3 branch for 5.4. The latest V30B3 is a diagnostic checkpoint, not a stable release.

## Verified ancestry (GitHub compare, base -> V30B3)

| Base | Ahead | Behind | Interpretation |
|---|---:|---:|---|
| main | 752 | 59 | Audit main-only commits |
| migration/blender-5.4-alpha-release-baseline | 545 | 4 | 214 changed files returned; release SHA is common ancestor |
| feature/pre54-symmetry-remesh-fixes-v01 | 518 | 1 | Audit unique commit |
| feature/sculpt-isolated-smooth-v01 | 756 | 21 | Audit unique changes |
| feature/zbrush-multires-layer-export-v01 | 752 | 51 | Audit unique changes |
| feature/native-multires-hidden-layers | 758 | 5 | Audit unique changes |
| experiment/delta-smooth-coarse-v01 | 756 | 22 | Audit unique changes |
| integration/xpbd-v12-polypaint-20261119 | 477 | 0 | Ancestor of V30B3 |

Counts are ancestry, not numbers of features or missing fixes. Compare results can cap file lists at 300; use local Git for complete inventory. Check all other branches and the user's local unpushed changes, including assets. Never cherry-pick all divergent commits blindly.

## Stage 0: actual upstream import
Run from a clean existing Git checkout on Windows, using the companion PowerShell script in this directory. It fetches the exact upstream SHA, verifies version 504, creates a separate stock worktree and writes inventories. No merge, reset, force push, or build is performed. Use -PublishUpstream only to publish the upstream reference after verification. The working physics checkout is not modified. Then create migration/54-port-core from the verified upstream branch; do not base it on the 5.2.3 inventory branch.

Before porting, build stock 5.4 in a distinct directory with Visual Studio 2022 / MSVC v143, appropriate libraries for that upstream commit and supported CUDA toolchain. Do not reuse 5.2.3 CMake cache. Preserve previous binaries and scenes. Check licensing and source provenance of external components.

## Stage 1: preservation and port order

1. Complete local inventory: release vs official 5.2.3, V30B3 vs release, main-only and other branch-only changes. Classify by module and disposition (keep/adapt/replace/retire with evidence). Find the upstream merge base and tag first; never assume entire upstream diff is custom.
2. DNA/RNA/versioning, .blend compatibility on copies, Sculpt Layers and HR Polypaint/Face Sets; sparse storage, hidden layers, base bake, strength/group semantics, Undo/Redo, edit-mode remapping, export.
3. CCG/Multires/PBVH dirty updates, commit-local paths, cache budgets, redraw and local normals. Adapt rather than overwrite upstream draw architecture. Keep validated fast paths until equivalent A/B.
4. Quick Pose, reference and posed/topological symmetry, gizmos, masked remesh, displacement UV alignment, low-level smooth/detail transport, Adaptive Stroke, custom brushes/assets and UI.
5. Port physical brush V30B3 separately; preserve prior validated behavior and compare with latest diagnostics. Test accumulated dabs using actual previous result, no one-dab reset, overlap, falloff, symmetry, mesh/grids, CUDA optional; maintain experimental opt-in.
6. Integrate and verify, including Linux later. No stable release until user approval.

Existing contracts: [Master roadmap](MASTER_ROADMAP_CODEX_20261108.md); [Performance preservation contract](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/03f1ea00dab3d4926d1477d8183491906418083f/doc/sculpt_extended/MIGRATION_54_PERFORMANCE_PRESERVATION_20261106.md). Preserve validated performance and behavior, not necessarily every previous algorithm.

## Stage 2: architecture for all future modules

Do not implement new large engines on the 5.2.3 inventory branch. After core validation, agree on shared contracts and small prototypes in separate branches from the same 5.4 integration base.

- Surface address: backend-specific stable sample identity (Mesh, CCG grid/patch coordinates, BMesh with generation/rebind), topology revision, rest/current pose frame; do not use transient triangle IDs as permanent identity.
- Surface field: typed channels (scalar, color, vector, categorical), ownership and sampling, sparse per-block storage, implicit zero, dirty ranges, precision, read/write/compose. Categorical Face Sets must not be interpolated as continuous floats.
- Transactions: preview vs committed data, Undo/Redo, invalidation, snapshots only for affected blocks, seam ownership, remap or explicit topology-change rejection; save/load versioning.
- Graphic engine: editable rectangle/ellipse/lasso/gradient SDFs, transform handles, visibility-aware projection, then surface Bézier, geodesic rope, pattern, Smart Region, liquify and extract. Reuse native gesture and expand. First targets mask and color, then Face Sets and geometry; fields can be passed to physical material profiles and GuidedQuad. Original Sculpt Mask Pro source not acquired; do not claim its code reused.
- Adaptive LOD (#8): separate display, evaluation and detail residency from authoritative geometry. Start display/compute experiments separately from mixed-resolution geometry. Preserve HR mask/color/detail/anchors across LOD changes, stitch seams, avoid full-mesh rebuild per dab.
- MicroDetail (#7): independent sparse microtiles keyed to stable surface patches; high-frequency scalar/vector displacement, resolution independent of working mesh, seam continuity, rest-frame transport, Undo, bake to texture/geometry. Not just a shader. Avoid assuming current sparse layers already provide this.
- NanoMesh (#9): stable anchor/frame, per-instance transforms, dirty chunks, density/erase/move/scale/rotate, curve distribution, mask/Face Sets, posed symmetry, independent render LOD, optional realization and bake. Compare Geometry Nodes, bke::Instances and chunk buffer at 100k instances / 100 edits per dab; no backend winner assumed.
- GPU acceleration only where benchmarked; CPU fallback and explicit dirty sync; avoid mandatory Warp dependency.

## Acceptance gates

A: release Windows 4 Nov; B: custom 5.4; C: stock pinned 5.4. Record exact SHAs, flags, fixture, driver, cold/warm, p50/p95, peak RAM/VRAM, Undo and draw. 100k/1M/10M/60M samples where feasible; stress separately. Check data integrity across save/reopen, Undo/Redo, low-high Multires, seams, masks, hidden data, layer strength, topology changes, pose and symmetry. Report static/build/runtime/benchmark results separately. Existing proposed thresholds (+10% median or +15% p95) are investigation triggers, not permission to lose performance. Do not claim a port complete until tested.

Next concrete gate: import exact upstream object, produce complete local inventories, compile stock 5.4. Then start a native port on separate integration branch.
