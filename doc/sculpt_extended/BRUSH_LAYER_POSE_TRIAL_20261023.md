# Optional layer sampling and Pose trial — 2026-10-23

## Approved version

### Preflight bug fixes

If no other effective layer is visible, Preserve Other Layers now uses native
sculpting without imposing isolation-only level, stroke-method, deformer or L0
restrictions. Native active-layer editability validation still runs. Hidden or
zero-strength other layers are skipped before inspecting their L0 offsets.
Restrictions remain when another effective layer actually needs isolation.

Windows Release rebuilt. Paired reproductions passed: lower level and Dots with
one effective layer were blocked before and finish after the fix; a hidden layer
with real nonzero L0 offsets plus another visible detail layer was blocked before
and finishes after. The L0 fixture verifies the actual x64 DNA offset payload
read-only, rather than inferring L0 edits from a changed aggregate layer hash.
Anchored Draw on 653,400 samples still produces meaningful geometry and restores
the protected surface within 1.2e-7. Startup/default checks passed. Evidence:
`C:/BlenderDev/LINUX-BUILD-20261023/guard-after/`, `hidden-l0-before-v3/`,
`hidden-l0-after/`, and `BRUSH-LAYER-POSE-20261023/guard-regression/`.

User approved the result on 2026-10-23 and requested Preserve Other Layers on by
default and publication on GitHub. It now defaults to true; an explicitly saved
per-brush false value remains a user override. Supported brushes: Draw, Clay,
Clay Strips, Clay Thumb, Inflate, Plane. Stroke methods: Space and Anchored at
the highest Multires level under the conditions described below. Pose is standard.
Earlier trial status and measurements below are retained as history.

## Subsequent user decision

Anchored extension installed and tested: allow the same private-position sampling
for Anchored strokes. Blender restores composed positions from the stroke Undo
snapshot before each preview; other-layer displacement caches stay constant.
Temporarily translate the brush reference center by the other-layer displacement
of the closest captured surface sample, restoring the actual anchor afterward.
This avoids missing the sampled surface with small anchors over raised detail.
Recalculate planar brush parameters in that reference frame. No live layer
visibility or persistent other-layer data is changed.

Draw, Clay, Plane and Clay Thumb Anchored off/on passed on 653,400 samples:
finite geometry, meaningful displacement >1e-5 (not just a changed float hash),
protected-surface error <=1.2e-7. Evidence in `anchored/RESULT.json` and snapshots.
Visual evaluation of textures, custom surface projection/transport options, and
symmetry combinations remains manual; these combinations were not benchmarked.

The user confirmed Preserve Other Layers works on the available brushes. Keep
that option and extend the same position-sampling path to Clay Thumb. The panel
is now **Brush Layers**. The Pose coarse-weight experiment and its UI option have
been removed: standard Pose smoothing is restored. The hidden-grid indexing fix
is retained. The trial description below records the earlier experiment, not
the controls in the updated build.

Local experimental build based on 433a48b865df4fe2ab753ddf259a8aa75b2fbb1a (5.2.3).
Not a validated performance release. Both new Brush properties default to false.

## Controls

Sculpt mode, sidebar Multires, **Brush Layer / Pose**:

- Draw, Clay, Clay Strips, Inflate, Plane: **Preserve Other Layers** subtracts
  other effective layers' displacement from private brush position samples.
  Live CCG positions stay composed; persistent non-active layers are not edited.
- Pose: **Gentle Multires Weights** smooths weights on a 17×17 lattice per grid,
  then interpolates weights to the full sculpt level. IK, deformation resolution,
  and layer destination are unchanged. Grab is unchanged.

Layer sampling requires highest Multires level, visible active layer with nonzero
strength, Space strokes, no live displacement/shrinkwrap, no L0 layer offsets,
and all effective layers stored at the current level. Unsupported combinations
are rejected. Shift Smooth uses the existing Delta Smooth path instead.

The position-only prototype still uses composed surface normals and ordinary
automasking. It does not guarantee perfect fine-detail preservation or behavior
identical to sculpting with all other layers hidden. Do not describe it as
universal layer isolation. Layer displacement maps are cached during the stroke;
cost grows with touched samples and effective layers.

Pose's optional proxy falls back to full-resolution smoothing when hidden-grid
storage is present, grid size is at most 17, or iterations are zero. A separate
hidden-grid indexing error in full-resolution scalar smoothing is corrected:
write the current sample index, not an outer grid-loop index.

## Evidence and limits

Synthetic cube fixture: 600 grids, size 33, 653,400 samples, one noise-detail
layer and one active working layer. Two five-sample strokes per combination.
Draw, Clay, Plane, Pose tested off/on; finite positions and a real geometric
change required. Hiding the working layer restores the protected surface within
1.2e-7 units. This is a surface tolerance check, not a byte-level layer payload
comparison. Clay Strips and Inflate have not received the same stroke test.

Representative end-to-end stroke timings, including ray queries and layer commit:

| Brush | Off (ms) | On (ms) |
| --- | --- | --- |
| Clay | 168, 151 | 162, 152 |
| Draw | 158, 143 | 158, 175 |
| Plane | 165, 149 | 177, 182 |
| Pose | 5171, 5145 | 5430, 5438 |

The Pose numbers above include a topology cache subsequently removed because it
did not improve total time. Earlier proxy-only measurements were 5236/5298 ms off
and 5223/5355 ms on: no established overall speedup. Fine-detail curvature
projection results are mixed and do not establish visual efficacy. The user
should compare the options on the actual dense sculpt before publication.

Evidence: `C:/BlenderDev/BRUSH-LAYER-POSE-20261023/`, including fixture, result JSON,
geometry snapshots, logs, and build logs. No public release or portable updated.
