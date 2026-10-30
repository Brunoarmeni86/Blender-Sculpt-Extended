# Sculpt Extended — verified features, usage and upstream research

**Updated:** 30 October 2026  
**Release:** Blender Sculpt Extended 5.2.3  
**Integrated source:** `cd0c653f83eeac4259f83ecbbeb0f1b84d3d14f9` (`checkpoint/stable-20261027`)  
**Release tag:** [`v5.2.3-sculpt-extended-20261030`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261030)

This document separates shipped code from upstream Blender work and from ideas that still require experiments. It is not a promise that every research item is already available in Sculpt Extended.

## Status legend

- **Implemented** — present in the integrated source above.
- **Validated** — implemented and covered by an automated or focused runtime test.
- **Upstream** — documented by Blender, but not necessarily backported or exposed by Sculpt Extended.
- **Research** — a candidate direction; not a shipped feature.

## What is implemented

| Area | Status | Current behavior | Main code anchor |
|---|---|---|---|
| Lower-level Multires Smooth | Implemented; profiled | Reuses a persistent high-resolution reference so a second Smooth stroke does not rebuild the entire reference before the first dab. The optimized path is deliberately narrow and falls back to Blender behavior outside its supported conditions. | `source/blender/editors/sculpt_paint/mesh/sculpt_layer_delta_smooth_v01.hh` |
| Shared Sculpt transform pivot | Validated | A Sculpt pivot can be independent or linked to the symmetry reference. Move, Rotate and Scale use the same reference without modifying geometry while positioning the pivot. | `scripts/startup/sculpt_extended_integrated/reference_native_snap.py` |
| Symmetry reference tool | Implemented | A persistent reference can be placed with snapping and transformed with `G`, `R`, `S`, axis constraints, numeric input, `Ctrl` snap and `Esc` cancel. | `scripts/startup/sculpt_extended_integrated/symmetry_reference_preview_transition.py` |
| Shape Only with protected mask | Implemented | Symmetry/remesh workflows can preserve the masked region while rebuilding the editable shape. Methods include Auto, Topology and Surface. | `scripts/startup/sculpt_extended_integrated/symmetry_shape_v2.py` |
| Protected masked remesh | Implemented | The Voxel Remesh integration protects masked coordinates/faces through the native remesh dispatch. Temporary trial caps and the experimental “try a finer size” rejection have been removed. | `scripts/startup/sculpt_extended_integrated/masked_remesh.py`, `source/blender/editors/object/object_remesh.cc` |
| Integrated startup and feature flags | Implemented | Loads the stable Sculpt Extended modules and their guarded native paths as a single build. | `scripts/startup/sculpt_extended_integrated/__init__.py` |

Removing experimental limits does **not** make remesh cost-free: very fine voxel sizes can still require substantial RAM and processing time. Blender may become temporarily unresponsive while the operation completes.

## Shared pivot: where it is and how to use it

The shipped interface labels are currently Italian.

1. Enter **Sculpt Mode**.
2. Open the 3D Viewport sidebar with `N`.
3. Open **Tool → Snap e pivot Sculpt**.
4. Enable **Mostra pivot trasformazioni**.
5. Enable **Condividi con simmetria** when the transform pivot and symmetry reference must be the same point. Disable it for an independent Sculpt pivot.
6. Use **Posiziona pivot con snap** to place it on a snapped target, **Usa riferimento come pivot** to copy the symmetry reference, or **Pivot al cursore 3D** to use the cursor.
7. Press `G`, `R` or `S`; constrain with `X`, `Y` or `Z`, hold `Ctrl` for snapping, and press `Esc` to cancel safely.

`G` moves the reference/pivot. `R` and `S` transform around that pivot. Locking, cancellation and shared/independent synchronization are protected so a cancelled modal operation restores the previous state.

This feature must not be confused with Blender 5.3's upstream **Transform Only Pivot** option. Blender's option changes how the standard transform pivot is repositioned; Sculpt Extended additionally links a persistent symmetry reference and a Sculpt transform pivot.

## Symmetry reference and Shape Only workflow

The **Riferimento simmetria** workspace tool is available in Sculpt and Edit modes. Place or snap the reference, then use normal modal transform controls (`G/R/S`, axes, numeric input, `Ctrl`, `Esc`). It can anchor to a mesh vertex or the 3D cursor and can be locked.

For protected operations:

1. Paint the mask that must remain protected.
2. Choose **Solo forma — maschera protetta**.
3. Select Auto, Topology or Surface according to the model.
4. Run the symmetry/remesh operation and inspect the mask boundary and preserved detail before continuing destructive work.

For Voxel Remesh, use **Proteggi zona mascherata**. Always save a copy before extremely fine remeshes; removing artificial preview caps intentionally exposes the real memory and compute cost.

## Validation evidence

### Shared pivot automated test

`SHARED-PIVOT-TEST.json` passed the following checks:

- shared reference-to-pivot synchronization;
- independent pivot behavior;
- no geometry change while positioning the pivot;
- gizmo registration;
- native Scale wrapper;
- protection of locked Scale state.

### Lower-level Multires Smooth profile

The focused comparison used `zombie dragon.blend`, object `Retopo_head.001`, with 424,456 grids, Multires maximum level 5 and the test at level 4.

- Legacy stable path: the second stroke spent about **58.8 s** in the Multires commit before its first dab.
- Persistent-reference preview path: the measured cold internal invoke was about **30.8 ms** in the comparable focused run.

These numbers demonstrate the removed rebuild in that scenario; they are not a universal benchmark. Results depend on mesh size, modifier stack, Multires level, memory pressure and brush configuration. The optimized Smooth path is guarded and uses the normal Blender fallback whenever its preconditions are not met.

## What official Blender research changes

### Blender 5.2

The official 5.2 Sculpt notes document the Scene Project brush, Add Primitive and interpolation of vertex/corner attributes by the voxel remesher. These are upstream Blender capabilities and should not be presented as original Sculpt Extended features.

### Blender 5.3

The official 5.3 notes document several relevant upstream changes:

- **Transform Only Pivot** in Sculpt Mode;
- generic mesh attributes displayed and interpolated under Multires;
- texture display for Multires sculpting;
- per-brush toggles for unified settings;
- custom curves for Hardness, Auto-Smooth and Spacing;
- rounded cursor display and cube brush tips.

Blender's compatibility schedule lists 5.3 as the November 2026 release series. Until a Sculpt Extended build is rebased and tested on it, these items are upstream comparison points, not claims about the current 5.2.3 package.

### Blender 2026 roadmap

Blender's official 2026 project overview describes work toward a better texture workflow and the beginning of layered textures, while explicitly indicating that a complete layered-texture solution is not expected to land in 2026. It also identifies Multires spike fixes. This supports research into texture/mask workflows and Multires robustness, but not a claim that those systems are complete here.

## Corrected research backlog

| Candidate | Status | Evidence and next proof required |
|---|---|---|
| Directional Smooth | Research | Discussed in the official Sculpt/Paint/Texture module meeting; behavior and UI were still ambiguous. Build a minimal brush prototype and compare it with ordinary Laplacian Smooth. |
| Independent texture and mask channels | Research / upstream direction | Consistent with the official texture-workflow direction. Requires data ownership, save/load, undo and Multires tests before integration. |
| Rake/roll mapping improvements | Research | Module feedback exists, but production behavior must be validated across symmetry, pen rotation and texture mapping. |
| Hard-surface Polish variants | Research | Candidate brush design only. Define measurable plane preservation, edge behavior and performance targets. |
| Vector displacement workflow | Research | Requires a stable tangent-space convention, import/export tests and Multires validation. |
| High-resolution attributes in Multires | Upstream 5.3; integration research | First verify Blender 5.3 behavior, compatibility and file round-tripping before considering a backport. |
| Sculpt performance/PBVH work | Research | Profile real scenes first. Accept changes only when CPU time, memory and visual output are measured against the integrated baseline. |
| Save and UI responsiveness | Research | Needs reproducible large-file traces and cancellation/undo safety tests; no speculative asynchronous write path should be advertised as shipped. |

## Acceptance rules for future integrations

Every candidate must provide:

1. a reproducible `.blend` file and exact steps;
2. a before/after profile or correctness test;
3. undo, cancel, save/load and crash-recovery coverage;
4. mask, symmetry and Multires coverage where applicable;
5. explicit fallback behavior;
6. a clear distinction between upstream code, local integration and original work.

## Primary sources

- [Blender 5.2 Sculpt release notes](https://developer.blender.org/docs/release_notes/5.2/sculpt/)
- [Blender 5.3 Sculpt release notes](https://developer.blender.org/docs/release_notes/5.3/sculpt/)
- [Blender release compatibility schedule](https://developer.blender.org/docs/release_notes/compatibility/)
- [Blender projects to look forward to in 2026](https://www.blender.org/development/projects-to-look-forward-to-in-2026/)
- [Sculpt, Paint & Texture module meeting — 3 March 2026](https://devtalk.blender.org/t/2026-03-03-sculpt-paint-texture-module-meeting/44512)

## Build integrity

The clean Windows release was built from the integrated source commit above and startup-tested. Published full archives are accompanied by `SHA256SUMS.txt` on the release page. A Linux package is a separate build artifact and must be reported as available only after its workflow and archive verification finish.

