# ZBrush Multires Layer Bridge — proof of concept (NOT integrated)

This folder contains a **tested, Blender-independent OBJ bundle writer** for the
first ZBrush bridge experiment. It is intentionally not registered in Blender,
does not change the current Sculpt Extended UI, and is not a working end-to-end
Multires exporter yet. See [design](../../doc/sculpt_extended/ZBRUSH_MULTIRES_LAYER_EXPORT_V01.md)
and [existing ZBrush roadmap issue #10](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/10).

## Run offline tests

From the repository root:

    python -m unittest discover -s tools/zbrush_bridge -p "test_*.py" -v

Tests cover deterministic point and face ordering, layer metadata, non-additive
results, invalid topology, NaN vertices, and no-overwrite behavior.

## Input contract

Call build_bundle(output_directory, baseline, layers, composed) with:

- baseline: highest-level evaluated mesh, **all geometry layers muted**
- each GeometryLayer.isolated: that **one layer at strength 1**,
  all other geometry layers muted; still a full absolute-position mesh
- composed (recommended): actual highest-level result at **original**
  visibility and layer strengths
- identical vertex indexing AND ordered polygon corner topology across every mesh
- identical object-local coordinate frame; no modifier-generated topology changes

The result is baseline_high.obj, layer_NNN.obj, optional
preview_composed.obj, and manifest.json. Output directory must not exist.

This format is for **geometry layers only**. A ZBrush native 3D Layer is not
encoded in OBJ; the OBJ file is imported into ZBrush *while recording* an
existing layer. The ZBrush point-order check and subdivision reconstruction are
still separate interactive validation steps.

## Codex integration TODO (do not treat as done)

- [x] Identify real source and RNA names: MultiresModifier.sculpt_layers,
  sculpt_layer_recording, active_sculpt_layer_index, and per-layer
  name/strength/visible. See source/blender/makesrna/intern/rna_modifier.cc
  and source/blender/blenkernel/BKE_multires_sculpt_layers.hh.
- [x] Implement reusable OBJ/manifest writer with ordered-topology, finite value,
  composition and no-overwrite checks; add offline tests.
- [ ] Implement **native evaluated Multires snapshot adapter** on a duplicate/
  temporary data evaluation path, not by changing live layer flags on the
  original object. RNA setters call commit and may alter REC or rebuild
  sculpt CCG; simply toggling original layers in a loop is unsafe.
- [ ] Commit pending Sculpt changes using existing guarded operation before
  snapshot acquisition. Reject unsupported modifier stacks.
- [ ] Verify correct CCG tessellation vertex order and no duplicates/seam
  mismatch at Lmax. Ensure the original layers, strengths, visibility, REC,
  level, source mesh and Undo state are unchanged after successful/failed export.
- [ ] Instrument memory/peak allocations: holding a full independent copy
  of *every* layer's high-res mesh is not suitable for 10M+ points. Stream
  each layer into the writer or extend it with an iterator after validation.
- [ ] Add an opt-in Export for ZBrush (Experimental) operator inside existing
  **Multires** UI only after native snapshot validation.
- [ ] Test real ZBrush import with at least two layers: create a layer at the
  highest level, enable REC, hide previous imported layers and import that
  layer's absolute-position OBJ. Check point identity and shape visually.
- [ ] Test Reconstruct Subdiv on a disposable ZBrush tool; it is
  topology-sensitive and is **not** guaranteed. Compare reconstructed Lmax
  vertex identity with the exact exported baseline.
- [ ] Validate tolerance for per-layer intensity, partial intensity, negatives,
  hidden layers, seams, boundary/tri/quad meshes, object scale, Undo, save/reopen.
- [ ] Investigate ZScript/GoZ automation *after* the manual ZBrush pass.

**Nothing in this folder has been run inside Blender or ZBrush.**
