# Sparse Sculpt Layers — architecture experiment

Start from frozen V05. This branch is for experiments, not a replacement of
the verified portable. Existing layers, Polypaint, Face Sets, Undo, spike
transport and native Multires behavior remain the comparison baseline.

## Compatibility contract

The working project keeps separate layers in the fork. A separate export/bake
operation must compose visible layers into native Multires MDisps in an output
copy and omit fork-specific layer payloads there. The native result must remain
usable in standard Blender. Verify opening it with an actual standard Blender
binary rather than inferring compatibility from the fork alone.

Polypaint is a separate export: Color Attributes or baked texture/UDIMs. Do not
promise that all maximum-resolution master samples fit in a base Color Attribute.

Conversion must use bounded temporary memory where possible; native persistent
Multires output still needs full dense storage. Streaming cannot remove this
minimum. Publish only when conversion is complete; failure leaves the original
project unchanged. Keep the original project with its layers.

## First experiment: isolated storage and composition

Use fixed-size sample blocks indexed by object/grid/level/block, with absent
blocks representing exact zero. Start with geometry-only additive deltas.
Do not retrofit Face Sets or color masters during this first experiment.

Prototype requirements:
- Preserve finite nonzero and subnormal values, and independent hidden data.
- Validate dimensions, level and finite inputs before publication.
- Snapshot immutable block payloads; copy only an edited block for Undo states.
- Compose a bounded batch into native grid output in the same layer order and
  float operation order as the current production implementation.
- Match V05 for visible/hidden layers, negative/zero strength and boundaries.
- Account for block metadata and Undo retention as well as payload bytes.

First gate: a standalone C++ prototype compared with dense composition on
randomized data, grid borders, repeated edits and snapshot restore. Measure
allocated bytes and wall time for sparse and dense detail. Reject a design
whose block overhead makes dense cases regress without a dense fallback.

## Integration gates

Only after the storage gate: adapt one existing sculpt stroke in a separately
built fork, with dense fallback. Then test level changes and subdivision support
closure; changed-region halos must be derived conservatively from actual
subdivision dependencies, not guessed neighbor rings.

Undo, serialization/migration, bake-to-native and actual stock-Blender opening
must each pass before the architecture can replace V05. Keep tests unified in
one runner for the user, with individual failure diagnostics inside its report.

Track separately: native persistent geometry, sparse layer payloads, evaluator
topology, temporary surfaces, cache retention and Undo. V05 shows that topology
reuse can improve time without proving lower RAM; block storage alone will not
remove the full native base evaluation cost.

Status: design and gates recorded; no new storage implementation or file format
is installed by this document.
