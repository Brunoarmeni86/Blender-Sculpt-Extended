# Approved Windows checkpoint — 27 October 2026

Blender Sculpt Extended 5.2.3. User-approved daily-use Windows checkpoint; custom Blender distribution, not an official Blender release. Desktop installation left unchanged.

Includes current Draw Transport/mesh support, Preserve Other Layers brush and tangent-buffer optimizations, optional Smooth Preserve (default enabled), mask master synchronization across Multires levels, sparse mask Undo at the current display level, stale composed-CCG write guard during Memfile Undo, first-stroke anchor for supported grid frames, and editable sidebar layer names.

## Evidence

- Real zombie dragon file: Retopo_head.001, 34,380,936 Multires samples; actual native Draw stroke, exact position hashes after Undo/Redo, mask hash and layer weights unchanged. Original file not saved.
- Lightweight mesh: 35 valid combinations passed, including saved L0 configurations, Draw/Smooth, Preserve on/off, weighted/negative/hidden layers, and three repeated Undo/Redo cycles. Coordinates compared with tolerance 2e-6, alongside mask hashes and metadata. These checks do not prove every possible Undo sequence.
- Prior targeted mask checks cover lower-level painting, Clear, L0 roundtrip, cross-level mask Undo/Redo and legacy data.
- Local first-stroke profiling retained equivalent output; no universal speedup claimed.

## Deferred investigations

- Mixed L0 -> L1 transaction did not restore the expected display level in one scripted sequence. Needs UI-equivalent isolated reproduction.
- L0 script-only unsaved RNA edits before the first stroke can revert to saved settings; configured/saved/reopened L0 cases pass. Do not equate this result with a confirmed UI regression.
- Draw Preserve at intermediate levels can reject the stroke under the highest-level guard. This is a current limitation, not a passed Undo test.
- Draw Transport retains known edge cases and requires its launcher environment switch. No new symmetry behavior promised.

Linux for this exact source is a separate build and must pass its own checks before publication. Earlier Linux assets are historical revisions. No physical GPU tests claimed.
