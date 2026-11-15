# Sculpt Layer sparse payload multi-cache — checkpoint 2026-11-15

**Status:** experimental candidate, Blender-runtime validated on a small fixture; not committed,
not released, and not yet benchmarked on a production-size mesh.

## Change

`LayerPayloadIndex` now retains up to four hot Sculpt Layers instead of only the last layer. Payload
mutation invalidates the matching entry, while global invalidation clears the bounded cache. This
targets repeated Strength edits that alternate between populated sparse layers without adding
full-mesh or per-dab work.

## Tests actually executed

- Visual Studio 2022 Release build: PASS. The existing build tree required `/Z7` because stale PDB
  state produced MSVC C1902; this is not a clean distributable build.
- `sculpt_layer_scale_benchmark.py`, 10/100/500 empty layers: PASS.
- `symmetry_strength_transition_probe.py`: PASS; no doubled Strength or surface drift detected.
- `sculpt_layer_multicache_probe.py`: PASS with two populated sparse layers and 16 alternating
  Strength transactions.

The direct trace showed two initial `BUILD` events (one per layer), followed by 14 `HIT` events.
With `BLENDER_SCULPT_LAYER_SUPPORT_CACHE=0`, all 16 transactions reported `OFF` and rescanned the
descriptors. Mean hot transaction time on this deliberately small fixture was 0.1862 ms with the
cache and 0.2297 ms with it disabled (about 18.9% lower). These sub-millisecond timings are noisy;
the trace is the stronger proof of avoiding repeated descriptor scans.

## Decision

**KEEP AS EXPERIMENTAL CANDIDATE.** The bounded behavior and invalidation paths compile and pass the
targeted runtime probe. Before promotion, repeat on large sparse Multires assets, test more than four
alternating layers (eviction), Undo/Redo, deletion/reordering, save/reopen, and compare RAM plus
p50/p95 timings against `v5.2.3-sculpt-extended-20261104`.
