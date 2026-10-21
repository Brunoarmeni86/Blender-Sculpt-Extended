# Blender Sculpt Extended

**An experimental Blender fork for non-destructive, high-resolution sculpting and painting.**

Blender Sculpt Extended extends the native **Multiresolution** workflow. Its main addition is **Sculpt Layers stored directly on Multires meshes**, complemented by **high-resolution Polypaint**: artists can refine geometry and color at different subdivision levels while preserving separate layer contributions and fine detail.

> **Project status:** Active development. This is an unofficial Blender fork, not a production-certified release. Features and test results described below refer to specific development checkpoints; the latest local Windows build may differ from the code currently pushed to GitHub.

## Key modifications

### 1. Non-destructive Multires Sculpt Layers

- Store sculpt displacement in **independent layers**, separate from the native Multires base.
- Add, duplicate, rename, reorder, hide/show and adjust the **strength** of layers; commit pending strokes without flattening the stack.
- Sculpt on the **base (L0)** or higher subdivision levels. Preserve fine detail when switching levels, within supported operations.
- Explicitly choose whether geometric strokes affect the **native Multires base** or an **active Sculpt Layer**.
- Preserve supported layer data through Edit Mode topology changes using persistent vertex/corner identity; surviving elements retain their data and new elements start with zero layer displacement.
- **Bake visible layers into the base**, keeping hidden layer contributions separate, subject to geometry and data restrictions.
- Additional layer operations (merge, flatten, groups and snapshots) exist in the codebase; coverage varies by operation and build.

### 2. High-resolution Multires Polypaint

- Maintain a **color master at the highest available Multires subdivision level** rather than permanently downsampling it when the sculpt level changes.
- View and paint colors at lower levels, including **L0**, while retaining fine-resolution color data.
- High-resolution **Paint, Blur and Smear** brush paths, with native brush options such as strength, flow, blend and wet-mix behavior.
- Multiple named color attributes, plus tools to select and remove high-resolution color data.
- Optimized color editing with localized redraw and experimental parallel/optimized Undo handling.

### 3. High-resolution Face Sets

- Face Set data and display paths adapted for Multires, including painting and visibility workflows across subdivision levels.
- Face Set brush support, **Box/Lasso/Line** gestures, hide/isolate/show controls and selected-ID operations.
- Experimental **Grow/Shrink, mask and high-resolution extraction** tools.
- L0 propagation and dense-region handling have dedicated implementations, but some display and editing edge cases remain under investigation.

### 4. Sculpt brushes and detail transfer

- **Draw Transport (Anchored):** reposition sculpted detail interactively, with controls for rotation, scale and strength.
- **Detail Transfer:** import a compatible, frozen displacement preview as a **new Sculpt Layer**, without overwriting existing layers or the base mesh.
- Experimental **Delta Smooth** for smoothing layer displacement, including lower-resolution sculpt contexts (opt-in).
- Additional integration paths for Multires displacement and shrinkwrap-to-layer workflows.

### 5. Baking and export workflows

- **Polypaint-to-UV image baking** from the retained high-resolution color master.
- Image export controls for resolution, margins and antialiasing; support for **sequential UDIM tile baking** in the UI.
- Multires **Bake Visible into Base**, with checks intended to protect hidden layers and reject unsupported data layouts.
- CPU sampling and GPU-related bake experiments are part of development; GPU acceleration is **not claimed as fully validated across devices**.

### 6. Performance, Undo and interface integration

- Sparse/implicit-zero displacement handling, localized update paths, and experimental region-aware Multires subdivision and caching optimizations.
- Changes to sculpt/paint Undo, save/reopen handling, and guarded operations for safer layer and color data updates.
- Native panels under **Sculpt Extended** and **Multires**, bringing layer, color, Face Set, transfer and baking controls together.
- A Windows native-build profile intended to run from `blender.exe` without an external feature-enabling launcher.

## Verification and limitations

Selected development checkpoints have passed focused Windows tests for layer preservation, L0 color editing, Undo/Redo, save/reopen and visible-layer baking. An optional subdivision experiment reported an **11.2% reduction** for one measured L4→L5 case; this is **not** a general performance claim or a demonstrated reduction in peak memory.

**Known limitations / current work:**

- Some recent local builds have shown **missing or incomplete Multires UI controls** and **Bake Visible failures**, including on new test files. These regressions should be rechecked against the exact source and executable before a release is tagged.
- Face Sets at L0, dense regions, Grow/Expand-related tools and some brush combinations still need broader regression testing.
- Operations may reject shared/linked meshes, shape keys, external or invalid Multires grids, certain topology edits, or unsupported modifier combinations.
- Performance and stability on extremely dense meshes, different GPUs and laptops are **not certified**.
- Files containing fork-specific data should **not be assumed to round-trip losslessly through standard Blender**. Back up projects before testing.
- The source repository and a successfully tested local executable are **not automatically the same build**; historical checksums should not be presented as checksums of a newly built binary.

## Development notes

- [Native Windows release candidate and validation](NATIVE_RELEASE_20261020.md)
- [Verified Windows source snapshot (19 October 2026)](VERIFIED_20261019.md)
- [Consolidated implementation notes](CONSOLIDATED_20261020.md)
- [Frozen V05 optimization checkpoint](FROZEN_V05_20261021.md)
- [Code reviews and open technical findings](CODE_REVIEW.md)

**Repository:** [Brunoarmeni86/Blender-Sculpt-Extended](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended)  
**Development branch documented:** `release/sculpt-extended-native-20261020`

## License and credits

Based on [Blender](https://www.blender.org/), developed by the Blender community. The Blender codebase is distributed under the **GNU General Public License v3 or later**, with individual files carrying their own compatible license notices. Blender Sculpt Extended is an independent, unofficial experimental fork and is not affiliated with the Blender Foundation.
