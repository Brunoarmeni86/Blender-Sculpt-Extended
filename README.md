# Blender Sculpt Extended

Custom Blender build for dense-mesh Multires sculpting, Sculpt Layers, Preserve Other Layers and Polypaint.

## Current approved checkpoint — Blender Sculpt Extended 5.2.3

[**Open the current release**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-optimized-20261028) · [Previous stable Windows and Linux](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-stable-20261027) · [All releases](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases)

| Platform | Download | Status |
|---|---|---|
| **Windows x64** | [**Portable ZIP**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-optimized-20261028/Blender-Sculpt-Extended-5.2.3-Windows-x64.zip) | Approved 28 October 2026; `233b1a2da92`; shared Multires indices and Smooth reads |
| **Linux x64 general purpose** | [**Previous stable tar.xz**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-stable-20261027/Blender-Sculpt-Extended-5.2.3-Linux-x64.tar.xz) | Source `419776d9b89`; Rocky 8/glibc 2.28; Ubuntu 24.04 startup verified; **does not include the latest Windows optimizations** |

Extract the complete ZIP and use **Avvia-Sculpt.cmd** to enable the approved optimizations and Draw Transport on OpenGL. Direct blender.exe retains the optional switches for rollback.

The checkpoint retains Multires mask persistence, layer Undo fixes and optional Smooth Preserve. New shared indices and Smooth reference reads passed controlled geometry/mask/Undo checks; the user's dense Zombie Dragon passed individual checks at 122,667,784 samples. Performance varies with scene and brush; no universal speedup is promised. Additional diagnostic experiments were excluded.

[Current details and known limits](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/233b1a2da926d9200da5321674e54038875813b1/doc/sculpt_extended/CHECKPOINT_20261028.md) · [Previous stable limits](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/419776d9b890811f73d7f68a9383f32bce982836/doc/sculpt_extended/STABLE_CHECKPOINT_20261027.md) · [Exact source](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/233b1a2da926d9200da5321674e54038875813b1)

Custom distribution, not an official Blender release. Approved checkpoint does not mean every bug is resolved. Previous releases remain available. Windows and Linux currently use different source revisions, identified above. Previous Linux includes CUDA/OptiX, HIP and oneAPI kernels; no new physical GPU testing is claimed.

Blender is distributed under the GNU GPL; see [COPYING](COPYING) and the source tree for license details.
