# Blender Sculpt Extended

**Blender Sculpt Extended 5.2.3 LTS** is an experimental custom Blender build for high-resolution Multires sculpting, editable Sculpt Layers, Polypaint HR, symmetry, and detail-aware deformation. It is **not an official Blender release**.

**New to Sculpt Extended?** Start with the **[simple step-by-step user guide](doc/sculpt_extended/USER_GUIDE_5_2_3.md)** — installation, Sculpt Layers, low-level Multires sculpting, Pose/Transform Blend, Polypaint, Face Sets, symmetry and troubleshooting. The guide covers the 4 November stable build and distinguishes experimental candidates.

## Download — latest Windows release (4 November 2026)

**[Blender Sculpt Extended 5.2.3 — release and downloads](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261104)**

| Platform / package | Status | Notes |
| --- | --- | --- |
| **Windows x64 — Standard** | **Latest release** | Uses the normal Blender Windows user configuration. |
| **Windows x64 — Portable** | **Latest release** | Includes the portable marker; preferences/configuration are stored next to Blender. |
| **Linux x64** | [Previous stable (27 October)](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-stable-20261027) | Older source revision; **does not contain the new 4 November Pose/Transform fixes**. |

Download the complete ZIP appropriate for your setup, extract it to a writable folder, and launch `blender.exe` from the extracted application directory. **Do not mix files from different release ZIPs.** Check `SHA256SUMS.txt` on the release page if you want to verify archive integrity. Back up important `.blend` files before trying this experimental build.

Source checkpoint for this release: `09a16d4b8482200101cb30728881ca4524edf019` (release tag: `v5.2.3-sculpt-extended-20261104`). Older builds remain available in [All releases](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases).

## What's new — 4 November 2026

- **Multires spike correction after Pose:** fixes the reproduced spike issue when sculpting/posing at a lower subdivision level and returning to the highest level.
- **Improved cross-level deformation transport:** lower-level corrections are transported in object space and encoded in the tangent frame of fine Multires samples, reducing damage to existing detail in the supported workflow.
- **Shared-pivot Move / Rotate / Scale:** refined falloff with **Transform Blend**, improved weighted rotation/scale interpolation, and topology-aware masked deformation.
- **Pose Segment Blend:** smoother transitions, including support for single-segment smoothing.
- **Pivot and symmetry workflow:** maintains the shared reference/pivot controls and snapping behavior.
- **Previous optimizations retained:** layer commits, sparse updates, memory-budget handling, mask persistence, and Undo-related fixes from earlier checkpoints.

These changes address tested paths; they do **not** imply that every Multires/Pose/layer combination is free of artifacts. See [4 November release notes](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261104).

## Feature overview

| Area | Included behavior | Status / caveat |
| --- | --- | --- |
| **Multires Sculpt Layers** | Create, select, rename, duplicate, hide/show, adjust strength, organize/merge and commit geometry layers; edit at supported Multires levels. | Implemented; not every brush/modifier combination is equally supported. |
| **Preserve Other Layers** | Supported Draw/Clay/Plane/Smooth workflows can reduce unintended changes to protected layers. | Guarded per-brush behavior; preserving stored layer data does not always guarantee identical visible detail under large deformations. |
| **Bake Visible Layers into Base** | Commit visible layer contributions to native Multires while retaining hidden layers (specific bake operator). | Check layer visibility and save a backup first. |
| **Multires masks and Undo** | Persistent high-resolution masking and improvements to layer commit, first-stroke Undo and level changes. | Optimized, but very large scenes can still require expensive rebuilds or Undo snapshots. |
| **Smooth / Delta Smooth** | Active-layer-aware smoothing, including optimized lower-level reference handling on supported configurations. | Falls back or rejects unsupported configurations; Multires-first modifier order is important for certain low-level paths. |
| **Pose & Transform** | Pose improvements, Segment Blend, shared-pivot Move/Rotate/Scale and Transform Blend. | New 4 November paths; validate detail on your own mesh. |
| **Symmetry reference** | Movable/snap-enabled sculpt symmetry reference; topology/surface and protected-mask workflows. | Implemented; **live posed symmetry for every brush dab is not yet complete**. |
| **Mask-protected remesh / Shape Only** | Protect masked shapes during supported symmetry/remesh operations. | Can be expensive at small voxel sizes; inspect boundaries. |
| **Polypaint HR** | Named high-resolution Color Attributes, Paint/Blur/Smear controls, persistent color masters and UV bake. | Single UV tile bake is available; not an automatic multi-UDIM pipeline or full color-layer compositing stack. |
| **Face Sets HR** | High-resolution Face Sets, visibility/masking controls and optional extraction. | Context, modifier and mesh-size limits apply; extraction can be feature-gated. |
| **Draw Transport** | Anchored Draw detail positioning with interactive rotate/scale/strength controls. | Specific supported Draw workflow, not universal detail transport for all brushes. |
| **Detail Transport Copy V01** | Preview copied/projected detail between supported sources and commit it into a new Sculpt Layer. | Experimental; test on small, aligned copies before production use. |
| **Displacement / Shrinkwrap → active layer** | Bake supported displacement or shrinkwrap results into the active layer; procedural displacement controls also exist. | Check mask, UV/mapping and modifier preconditions. |

Blender's ordinary sculpt brushes, VDM/Elastic tools, Multires, and QuadriFlow are part of the Blender base where applicable; they should not all be interpreted as new original Sculpt Extended features.

## Quick start — Sculpt Layers & Multires

1. **Prepare a mesh.** In Object Mode, select your mesh and add a **Multiresolution** modifier. Subdivide to the level your hardware can handle; save a backup copy.
2. **Enter Sculpt Mode.** Open the 3D Viewport sidebar with **N**, then use the **Multires** tab and/or the modifier controls. Some tools are available only once Multires grids exist.
3. **Create a Sculpt Layer.** Use the Sculpt Layer controls to add a new layer, set it active, and give it a descriptive name. Sculpt on that layer rather than modifying the base when you want an editable contribution.
4. **Blend and inspect.** Adjust the layer's **Strength** and **Visibility** to compare its contribution. Duplicate or organize layers if needed; keep protected layers visible when checking the final sculpt.
5. **Change Multires level carefully.** Work at a lower Sculpt level for broad edits, then return to the highest level to inspect fine detail. The November 4 Pose fix specifically targets spikes in this workflow.
6. **Commit when needed.** Use **Commit** before operations that require finalized grid data or when the UI reports pending layer geometry.
7. **Bake only intentionally.** **Bake Visible Layers into Base** transfers visible contributions into native Multires and preserves hidden layer data in the supported operator; it is different from simply applying the Multires modifier. Save a separate file first.

**Smooth / layer protection:** for supported brushes, open **N → Multires → Brush Options** and enable **Preserve Other Layers**. When using low-level Smooth, a single Multires modifier placed first in the stack is the safest tested setup. Unsupported stacks may fall back or be rejected.

## Quick start — Pose, Transform Blend and shared pivot

1. With the Multires mesh in **Sculpt Mode**, choose the **Pose** brush. For broad deformations, work on a lower subdivision level and then return to your detailed level to inspect the result.
2. In the Pose brush settings, use **Segment Blend** to soften transitions between affected areas. The 4 November build also supports the single-segment smoothing path.
3. For sculpt transforms, open **N → Tool → Snap e pivot Sculpt**.
4. Turn on **Mostra pivot trasformazioni**. Keep **Condividi con simmetria** enabled to reuse the symmetry reference, or disable it for a separate transform pivot.
5. Click **Posiziona pivot con snap** to set the pivot; use **G** to move it, **X/Y/Z** to constrain, **Ctrl** for native snapping and **Esc** to cancel. **Usa riferimento come pivot** or **Pivot al cursore 3D** are alternative placement methods.
6. Use sculpt **Move / Rotate / Scale** with the shared pivot. Adjust **Transform Blend** / falloff in the relevant tool settings and inspect masked or layered regions at full resolution.

**Important:** the new Pose transport fix is not a full implementation of ZBrush-style Poseable Symmetry. Live stroke symmetry on an already posed mesh remains a development priority.

## Quick start — symmetry and masked operations

1. Enter **Sculpt Mode** or **Edit Mode** and choose **Riferimento simmetria** (symmetry reference) where available.
2. Position the reference with snapping; its transform supports **G/R/S**, axis constraints, numeric input, **Ctrl** snapping and **Esc** cancellation.
3. To protect part of a mesh, paint a **Mask** over the region you want to keep, then use **Solo forma — maschera protetta** and choose **Auto / Topology / Surface** as appropriate.
4. For masked Voxel Remesh, use **Proteggi zona mascherata**; inspect the boundary afterward. Very fine remesh values can consume substantial RAM and processing time.

Symmetry reference tools and topology-based operations are **not** a guarantee of perfect symmetry on arbitrary posed or unevenly retopologized models.

## Quick start — Polypaint HR and Face Sets HR

### Polypaint HR

1. On a Multires mesh, open the **N → Multires → Color Attribute HR** tools. Create a compatible **Nuovo Color Attribute** (or select an existing attribute). Multiple named HR attributes are supported.
2. In **Polypaint Brushes**, select **Paint**, **Blur** or **Smear** and adjust strength, radius and applicable brush controls. **Shift** invokes Blur in the documented paint workflow.
3. Select the correct UV map by name before using the **Polypaint UV Bake V01** operator; save the resulting image externally. V01 handles **one UV tile per bake**, not full UDIM export.
4. Removing an attribute is destructive to that attribute's HR color data; **Rimuovi tutti i colori HR** clears all and requests confirmation.

### Face Sets HR

1. With a compatible Multires mesh in **Sculpt Mode**, open **N → Multires → Face Sets HR**.
2. Use **Attiva pennello Face Sets HR** to paint, or the **Box / Lasso / Line** tools. Choose an **ID Face Set** to hide, isolate, show, grow, shrink, or convert to mask.
3. **Extract Face Set HR** requires a supported configuration and Sculpt level **L1+**; commit pending layer changes first. Some builds require opt-in flags to activate HR Extract.
4. This panel currently requires **exactly one Multires modifier and no other modifier**; it also enforces a high-resolution mesh-size limit. If the panel reports an unsupported mesh, the operation is unavailable on that object.

## Quick start — detail transport and baking

- **Anchored Draw Transport:** in **Sculpt Mode**, open **N → Multires → Brush Options → Activate Draw Transport**. The tool activates Anchored Draw. Use **R** to rotate, **S** to scale and **SS** to change strength during the supported transport interaction. It is also exposed as the **Draw Transport** brush option.
- **Detail Transport Copy V01 (experimental):** choose a source and target in the **Detail Transport Copy V01** panel, adjust projection constraints, run **Crea anteprima separata**, inspect the preview, then use **Conferma in nuovo layer** only after checking the result. Use test copies of aligned meshes.
- **Displacement / Shrinkwrap bake:** select an active Multires Sculpt Layer and use the corresponding bake-to-active-layer operator. Verify masks, modifier order and mapping first; keep an unbaked source copy.
- **Bake Visible Layers into Base:** use the dedicated Sculpt Layer bake command after **Commit**, not the generic Apply Modifier operation; hidden layer contributions are intended to remain separate.

## Experimental features, limits and roadmap

Not all features present in the source are enabled or production-ready in the released binaries. In particular, HR extraction, some Detail Transport/Delta Smooth combinations, and advanced feature-flag paths have explicit preconditions. Large Polypaint/Undo operations can still trigger expensive global Multires rebuilds. Back up important files and report precise reproduction steps when something fails.

**Planned rather than shipped as complete features:**

- **Poseable Symmetry / Quick Pose:** continuous posed-mesh symmetry and reliable visual transport of other layers during arbitrary brush strokes.
- **MicroDetail:** compact editable microgeometry beyond ordinary full-resolution Multires grids.
- **Adaptive Sculpt LOD:** mixed-resolution/detail residency per region; existing surface projection is **not** adaptive LOD.
- Future possibilities: Morph Brush, geometry-layer blend modes, color-layer compositing and guided retopology. These are **not** claimed as finished in the November 4 build.
- **NanoMesh Sculpt Layers (proposed):** Mask/Face Sets-controlled placement brush, persistent instances, Multires-aware anchoring, Vector Displacement and Hybrid outputs usable beyond Sculpt Mode. **Design only, not included in the released binaries**; see [design spec](doc/sculpt_extended/NANOMESH_SCULPT_LAYERS_ARCHITECTURE_20261106.md) and [issue #9](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/9).
- **ZBrush-inspired portable tools (proposed):** PolyGroups/Face Sets operations, Rake/Roll, hard-surface brush presets, Morph Brush, Selection Sets and guided retopology; see [feature backlog](doc/sculpt_extended/ZBRUSH_PORTABILITY_FEATURE_BACKLOG_20261106.md) and [issue #10](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/10).

Further technical references:
- [NanoMesh Sculpt Layers / VDM / Hybrid architecture (6 November)](doc/sculpt_extended/NANOMESH_SCULPT_LAYERS_ARCHITECTURE_20261106.md)
- [ZBrush portability and sculpt brush backlog (6 November)](doc/sculpt_extended/ZBRUSH_PORTABILITY_FEATURE_BACKLOG_20261106.md)
- [Existing-feature audit (4 November)](doc/sculpt_extended/ADDITIONAL_EXISTING_FEATURES_AUDIT_20261104.md)
- [SculptForge / ZBrush / forum comparison](doc/sculpt_extended/COMPARATIVE_AUDIT_SCULPTFORGE_ZBRUSH_FORUMS_20261104.md)
- [Feature deep dive and UI guidance](doc/sculpt_extended/FEATURE_DEEP_DIVE_20261030.md)
- [Pose / MicroDetail / LOD roadmap](doc/sculpt_extended/ROADMAP_POSE_MICRODETAIL_LOD_20261103.md)
- [Previous stable checkpoint and known limitations](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-stable-20261027)

## Reporting issues

When reporting a bug, include the **release tag/build**, OS, modifier order, Multires current/total levels, whether layers/masks were active, exact steps (especially Pose → higher level), and whether Undo/Redo or save/reopen changes the behavior. A small, non-sensitive reproduction file is ideal.

## License

Blender and this source distribution are governed by the applicable GNU GPL terms. See [COPYING](COPYING) and the source headers. **Blender Sculpt Extended is an independent custom build, not an official release from the Blender Foundation.**
