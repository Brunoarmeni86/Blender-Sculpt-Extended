# Approved optimization checkpoint — 28 October 2026

Based on stable source419776d9b89. Approved by the user after local trials on dense Zombie Dragon scenes. Custom Blender5.2.3 distribution, not an official Blender release.

## Changes

- Optional OpenGL sharing of immutable, compatible Multires grid triangle index buffers. Hidden-grid and non-OpenGL paths keep previous behavior. Subranges retain existing node invalidation and source lifetime ordering.
- Correct OpenGL vertex-buffer accounting on repeated updates and release; this accounting fix alone is not a physical RAM saving.
- Optional direct Smooth reference reads only for completely initialized dense same-grid storage. Sparse, partial and seam-neighbor reads retain previous lookup. Arithmetic and accumulation order unchanged. Native Smooth and mesh L0 dispatch unchanged.

Use Avvia-Sculpt.cmd in the portable package. It sets BLENDER_MULTIRES_SHARED_INDEX_V01=1, BLENDER_SCULPT_DELTA_GRID_READ_V01=1 and the existing Draw Transport switch, and selects OpenGL. Direct blender.exe retains the opt-in switches; this is intentional rollback capability, not a promise that settings are persisted in blend files.

## Validation

Controlled grid-index tests: exact geometry/mask/layer output during level switches and strokes; exercised smooth/flat, hidden/shown, wire and Undo/Redo. Masked silhouette variation also occurs with the old path and is not claimed pixel-exact. Controlled index residency decreased from255531696 to144048bytes in that scene; this is not a universal memory figure.

Smooth and Shift+Smooth controlled fixture: bit-exact geometry OFF/ON, protected layers/masks unchanged, exact Undo/Redo. Dense Zombie Dragon at122667784 samples: individual native-event checks pass geometry Undo/Redo and masks; source not saved. Cross-run event strokes are not deterministic, so exact cross-run geometry and universal speedup are not claimed. Native Smooth also has baseline run-to-run numerical variation.

Additional zero-sample and preparation timing experiments were not adopted. Per-stroke prepared-grid reuse works; no new cross-stroke cache or layer-payload sharing introduced. Existing stable checkpoint limitations, including deferred mixed L0 transactions and Draw Transport edge cases, still apply; see STABLE_CHECKPOINT_20261027.md. Linux remains on its previous source until separately rebuilt and verified. No physical GPU testing beyond this local Windows/OpenGL setup is claimed.
