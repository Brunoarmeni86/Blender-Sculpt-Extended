# Conform: two explicit fitting targets

Baseline: 83d81b2dfc2af8ba7763d33fce212dbaa9b5dc49.

The existing multiresModifier_conform_layers(object, mmd, include_visible_layers)
core already shares validation, staged rebasing, and surface-preservation checks.
The base target uses native cage fitting; the final target includes visible
weighted Sculpt Layers. Both rebase every layer, including hidden layers.

This candidate changes only MOD_multires.cc panel routing:

- Empty stack: Conform Base retains OBJECT_OT_multires_base_apply with
  apply_heuristic=false.
- Nonempty stack: Conform Base invokes OBJECT_OT_multires_conform_layers with
  include_visible_layers=false; Conform Layer-Aware invokes the same operator
  with include_visible_layers=true.
- Both layer-preserving buttons remain Object Mode only and experimental.
- Apply Base retains its existing separate operator and restrictions.

The redundant Without Layers button is replaced by Conform Base. This name
means the fitting excludes layer contributions; it does not delete layers.
No changes to fitting, sparse storage, REC behavior, undo, or memory allocation.

Validation: compared the panel's two dispatch choices with the baseline core
and operator property defaults. Native Blender compilation and UI/runtime
validation are pending. Test each target on a separate copy of the same scene,
check base cage differences, visible final surface preservation, hidden layers,
strength changes, and Object Mode Undo/Redo. The separate Edit/Sculpt undo issue
remains open and is not addressed by this UI candidate.
