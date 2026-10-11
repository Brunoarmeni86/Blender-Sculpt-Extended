# Edit Mode topology remap V0.1

Base: `83d81b2dfc2af8ba7763d33fce212dbaa9b5dc49` (stable Windows baseline).

Latest recovered user test (2026-10-11): moving existing vertices in Edit Mode works;
Delete+Fill, Extrude and Dissolve leave Sculpt Layers incompatible with REC. Older
RC14.1 aggregate test claims do not establish that the current sparse baseline passes.

## Changes

- Seed one-based hidden corner IDs before conversion to BMesh. Zero is unassigned.
- Store cyclic neighbors and face size; retain only unique corner identities with unchanged
  local orientation. Changed or ambiguous face corners get zero layer delta. This is a
  conservative fallback, not high-resolution reprojection.
- Load back L0 and high-resolution storage at the Edit Mode boundary, rather than remapping
  inside the REC setter. An unchanged topology leaves recording and high-resolution buffers intact.
- Treat null layer payloads as implicit zero. Transfer surviving payload ownership, without
  copying all high-resolution data. Preserve separately remapped L0 deltas.
- Validate color/Face Set metadata before modifying the high-resolution layer arrays.
- When saving/rendering without leaving Edit Mode, synchronize BMesh IDs and only the newly
  reset native displacement grids, so the next flush does not replay stale identities/data.

No sampling, Conform, subdivision algorithms or REC RNA setter changes are included.
This does not establish regression safety for those paths: the integration tests below remain required.

## Validation completed here

`python tests/python/sculpt_extended_multires_layer_remap_test.py`

Seven C++ cases passed against the actual `remap_surviving_corners` and `free_layer` bodies,
compiled with AddressSanitizer and UndefinedBehaviorSanitizer and minimal storage adapters.
LeakSanitizer is disabled because this managed sandbox cannot enumerate process tasks.
These checks cover sparse grids, dense ownership, retained L0 data, duplicate maps,
preflight failures for color/Face Sets, initialized new Face Sets, and recording state.
Against the original baseline source, the sparse-survivor regression fails before any
payload duplication. The optional `--source` argument reproduces that comparison.

Windows compilation exposed an invalid zeroed allocation of non-trivial Face Set grid
objects. The follow-up uses constructor-aware `MEM_new_array`, whose in-class defaults
initialize new grids. The standalone allocator adapter now enforces the same trivial-type
restriction to catch this class of error. Full Windows compilation remains pending.

The same seven native GTests are registered in blenkernel CMake. They have **not** been
compiled against Blender here. The standalone adapters do not validate Blender API/ABI,
BMesh conversion, dependency-graph rebuilds, undo, or visual results.

## Required Windows build/runtime matrix

1. Compile this branch, then run the `MultiresLayerRemap.*` native GTests if WITH_GTESTS is enabled.
2. Existing stable scene: REC -> sculpt -> Commit -> Edit Mode vertex move -> Sculpt.
   Verify retained layer buffers/deltas and REC behavior against stable main.
3. Repeat with Delete+Fill, Extrude, Dissolve, face flip, duplicate face and same-count topology edits.
   Verify unchanged regions retain each layer; changed/ambiguous corners start zero.
4. Save twice while still in Edit Mode, exit, reopen and enable REC. Test Undo/Redo separately.
5. Sculpt -> Commit at lower and maximum Multires levels; L0 with retained cage deltas.
6. Strength/visibility changes, Subdivide/Unsubdivide/Delete Higher, Conform Base and
   Conform layer-aware, Texture/Shrinkwrap bake, Polypaint and Face Sets.
7. Large mesh: verify no full layer payload copies during topology migration and no
   topology migration/rebuild triggered by REC alone.

This branch is an uncompiled integration candidate until that matrix passes. Region-local
reprojection for changed existing topology is a separate follow-up, not implemented here.
