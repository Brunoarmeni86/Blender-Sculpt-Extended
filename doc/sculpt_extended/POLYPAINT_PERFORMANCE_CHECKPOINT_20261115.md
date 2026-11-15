# Polypaint HR performance checkpoint — 15 November 2026

Status: **BLENDER-RUNTIME-VALIDATED candidate**, not merged and not released.

## Implemented

- Rectangle-only staging for already materialized color grids.
- Parallel base-color initialization with one base patch lookup per grid.
- Sparse regional Color Undo with exact overlapping-dab coverage.
- One-bit Undo coverage map instead of one byte per master sample.
- Reusable per-stroke staging buffers, capped at 32 MiB.
- Reserved local containers to avoid repeated growth during a dab.
- Local validation of touched grids instead of a full-master scan per dab.
- Cage-only topology and mesh-attribute setup is skipped at higher Multires levels.
- Parallel row projection for coarse-to-master color updates.
- Per-dab diagnostic output is disabled unless
  `BLENDER_SCULPT_POLYPAINT_V04_DIAGNOSTICS=1`.

## Tests actually executed

Build: Visual Studio 2022 / Release, Blender Sculpt Extended 5.2.3 LTS.

- Paint, Blur and Smear at L0, L1, L2, L3 and L4: PASS.
- Exact Undo/Redo, including master presence and L0 base color: PASS.
- More than one million master samples: PASS.
- All Paint blend modes, Density, Wet Mix and Wet Persistence: PASS.
- Fully masked Paint/Blur/Smear at L0-L4: PASS.
- Hidden samples across high and cage levels: PASS.
- Cage boundary fixture with 24,576 grids: PASS.
- Memory-budget abort before publication: PASS.
- Multiple color attributes, remove, Undo/Redo and same-name reuse: PASS.
- Save/reopen exact HR colors and protected domains: PASS on the regional Undo checkpoint.

Result directories are under `C:\BlenderDev`:

- `POLYPAINT-UNDO-REGION-TEST-20261114`
- `POLYPAINT-PARALLEL-PROJECTION-LARGE-20261115`
- `POLYPAINT-FINAL-MATRIX-20261115`

## Deliberately not implemented

- GPU color-tile invalidation: Blender 5.2 exposes PBVH node-level attribute invalidation,
  not a safe public tile-level update path. Rewriting the draw cache is deferred to the
  5.4 PBVH migration and requires a separate benchmark.
- Automatic prewarming when Polypaint is selected: this only moves the pause, can allocate
  untouched masters, and is therefore not enabled by default.
- Skipping normal Multires CCG rebuilds during a real level change: geometry still changes
  with the level, so suppressing that rebuild would risk stale geometry. Color-only strokes
  remain on the local attribute-update path.

Decision: **KEEP as experimental checkpoint**. Interactive p50/p95 timing on the user's
largest production mesh remains useful, but the automated data-integrity matrix is green.
