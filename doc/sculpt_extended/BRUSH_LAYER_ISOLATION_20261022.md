# Brush layer isolation audit

Delta Smooth smooths the active layer offset relative to a captured reference,
then reconstructs the visible result. Its begin hook covers Laplacian Smooth,
Shift Smooth and the Auto Smooth component of other brushes. It does not isolate
the main operation of every geometric brush. Existing restrictions include live
displacement/shrinkwrap and nonzero active-layer L0 cage edits.

The common Multires commit subtracts the other effective layers and publishes
only the active layer's payload. This protects their stored data, but a brush
working on the composed surface may counteract their visible detail by writing
opposite offsets into the active layer. Storage isolation is not detail isolation.

Extending the Smooth behavior is feasible but requires brush-specific work:

- Draw/Inflate: choose reference normals and displacement semantics.
- Clay/Flatten/Fill/Scrape: estimate the target plane from the intended layer
  surface, avoiding cancellation of detail from other layers.
- Grab/Elastic/Pose/Rotate: transport existing detail with the deformation;
  subtracting a fixed reference alone does not transport it.
- Mask, Face Sets and color brushes edit separate domains, not geometric layers.

No universal layer-only brush behavior is included in this release. Manual
validation of those semantics remains separate from the confirmed shading fix.

The attempted end-of-stroke commit in 11ed458a4b4 used base_modifier(), which
returns null for regular positive-level Multires Sculpt. It therefore did not
establish the claimed general behavior. That block is removed; established
commit/Undo boundaries are retained instead of adding an unverified boundary.
