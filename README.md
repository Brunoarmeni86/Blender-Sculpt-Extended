# Blender Sculpt Extended

Custom Blender build for dense-mesh Multires sculpting, Sculpt Layers, Preserve Other Layers and Polypaint.

## Current stable checkpoint — Blender Sculpt Extended 5.2.3

[**Open the stable release**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-stable-20261027) · [All releases](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases)

| Platform | Download | Status |
|---|---|---|
| **Windows x64** | [**Portable ZIP**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-stable-20261027/Blender-Sculpt-Extended-5.2.3-Windows-x64.zip) | Approved 27 October 2026; `419776d9b89` |
| **Linux x64 general purpose** | [**Portable tar.xz**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-stable-20261027/Blender-Sculpt-Extended-5.2.3-Linux-x64.tar.xz) | Same source `419776d9b89`; Rocky 8/glibc 2.28; Ubuntu 24.04 startup verified |
| Older Linux builds | [Historical releases](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases) | Earlier revisions; not equivalent to the current Windows |

Extract the complete ZIP and use **Avvia-Sculpt.cmd** to include Draw Transport. `blender.exe` also starts directly; Draw Transport needs the launcher environment switch.

The current checkpoint includes Multires mask persistence, layer Undo fixes, optional Smooth Preserve and the current brush optimizations. Actual geometry Undo/Redo was checked on the user's dense Zombie Dragon and in 35 valid lightweight combinations. Stable means the user-approved checkpoint, not a claim that all bugs are resolved.

[Known limits and verification details](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/419776d9b890811f73d7f68a9383f32bce982836/doc/sculpt_extended/STABLE_CHECKPOINT_20261027.md) · [Exact source](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/419776d9b890811f73d7f68a9383f32bce982836)

This is a custom distribution, not an official Blender release. Previous versions remain available. Windows and the current Linux package share the same source. Historical Linux packages are earlier revisions. CUDA/OptiX, HIP and oneAPI kernels are included in the current Linux package; no physical GPU testing is claimed.

Blender is distributed under the GNU GPL; see [COPYING](COPYING) and the source tree for license details.
