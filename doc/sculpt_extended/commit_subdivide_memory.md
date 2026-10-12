# Commit and empty-layer Subdivide memory candidates

Parent fa4998d: cache-only candidate, itself provisional and reversible. Explicitly
exclude the rejected strength-zero correction. Two isolated production commits;
main and bake/sampling-quality untouched. Absent flags preserves prior routes.

## Commit: avoid unused original grid copies

multiresModifier_reshapeFromCCG stores a deep copy of every original displacement
and paint-mask grid. multires_reshape_smooth_object_grids_with_details returns
immediately when reshape.level == top.level. The assignment from CCG and the
object-to-tangent conversion do not read orig grids; context cleanup supports
null original buffers. Under BLENDER_SCULPT_COMMIT_SKIP_ORIGINALS=1, skip only that
copy at equal levels. Lower-level detail propagation still stores originals.

Potential saving for N copied top-level samples: 12*N bytes of original float3
payload, plus 4*N if an original paint-mask payload is copied. For 60 million
copied samples that is about 720 MB plus optional 240 MB, excluding descriptors.
Actual sample counts include duplicated grid boundaries; these are estimates,
not native memory measurements. The scratch mesh, CCG, layer data, conversion
work and Undo storage remain. The ordinary local Commit solver is unchanged.
The same explicit opt-in also affects equal-level native base reshape, not Conform.

## Subdivide: first structural sparse shortcut

The previous layer subdivision pipeline materializes every positive-level layer
and stores a full subdivided base-plus-unit-layer scratch surface for each one,
even when no grid has a delta payload. Under BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE=1,
a structurally empty layer skips materialization and surface construction.
After every ordinary layer surface is validated and the sculpt session is flushed,
update empty layer descriptor dimensions/level only; payload stays null.

Qualification requires valid positive-level descriptors, no grid payload, no
hidden bitmap, and no nonzero L0 contribution. Visibility/strength alone never
qualify a layer as empty. Partially painted layers retain global native smoothing
and seam propagation. Previously densified numerical-zero layers are not scanned
at full resolution or treated as empty. This is intentionally narrower than
per-grid sparse subdivision, which requires a separate propagation proof.

Failure before publication leaves the empty layer descriptor level unchanged.
Native base subdivision and all ordinary-layer staging remain. This does not
stream all populated-layer results, reduce every scratch surface or rewrite
resample_all. Further memory work should preserve its transactional publication.

## Minimal validation

Extracted actual source routing/helper check with g++ warnings-as-errors passes:
flag off/on equal-level copy gate, lower-level preservation, failure cleanup,
empty/partial/hidden/L0 qualification and null metadata publication. Patch apply
and reverse checked against exact source. These are policy checks with mocked
services, not native OpenSubdiv, shape or Undo tests.

Windows native compilation pending. A small background cube fixture uses the
verified sculpt.multires_layer_add and object.multires_subdivide operators, then
compares finite evaluated geometry and stack/level after save/reload for flag
OFF/ON. It has not run here. A global Commit test requires a fresh process with
BLENDER_SCULPT_LOCAL_COMMIT=0, a small scene copy, one native stroke and Commit;
otherwise the local solver returns before this reshape route. Logs show actual
route. Do not claim memory reduction/speedup or safe extreme resolution until
native results support it. Install/rollback reuses the existing shareable build.
