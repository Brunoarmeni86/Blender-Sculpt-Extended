# ZBrush Multires + Sculpt Layers bridge — V01 technical plan

Status: **research / first implementation branch, NOT a working exporter**  
Target: Blender Sculpt Extended 5.2.3 native Multires delta Sculpt Layers -> ZBrush desktop 3D Layers.  
Branch: `feature/zbrush-multires-layer-export-v01`.

## Verified external behavior

- ZBrush permits `Tool > Import` of a same-vertex-count, same-point-order OBJ while a newly created 3D Layer is in **REC**. The deformation is captured on that layer. References: https://www.zbrushcentral.com/t/creating-a-layer-or-morph-target-from-a-separate-mesh/329756 ; https://www.zbrushcentral.com/t/import-as-layer-answered/316035
- ZBrush layers must be *created* at the highest subdivision level. A recording layer can then be edited at lower levels, but recording is ended at highest: https://help.maxon.net/zbr/en-us/Content/html/user-guide/3d-modeling/sculpting/3d-layers/3d-layers.html
- `Tool > Geometry > Reconstruct Subdiv` *attempts* to recover lower levels from a high-res quad mesh; success is topology-dependent, not guaranteed: https://help.maxon.net/zbr/en-us/Content/html/reference-guide/tool/polymesh/geometry/geometry.html
- ZBrush's FBX exporter supports 3D Layers as blendshapes; its import documentation does NOT promise to rebuild native 3D Layers from FBX blendshapes. Do not use FBX as the primary Blender -> ZBrush layer interchange: https://help.maxon.net/zbr/en-us/Content/html/user-guide/zbrush-plugins/fbx-exportimport/fbx-exportimport.html
- GoZ is transfer automation, not an additional interoperable layer format: https://support.maxon.net/hc/en-us/articles/7945725232796-GoZ-with-Non-Supported-Applications

## Required behavior / non-goals

V01 MUST preserve separate **geometry** layers, original names and requested strengths/visibility (via manifest and manual ZBrush setup initially), high-res surface geometry, and deterministic topology/index order. Never bake, merge, or apply a modifier to the original source as an export side effect. A complete native ZBrush subdivision hierarchy is an **experimental second stage**; V01 must not claim that reconstructing levels always succeeds. Polypaint/HR Face Sets/masks/UDIM and a fully automatic GoZ round trip are out of scope for V01.

## Proposed interoperable bundle

```
Creature_zbridge/
  baseline_high.obj            # native Multires base with geometry layers muted
  layer_001_Creases.obj        # absolute positions: baseline + only Creases at strength 1
  layer_002_Scales.obj         # absolute positions: baseline + only Scales at strength 1
  preview_composed.obj         # reference: original sculpt at original visibility/strengths
  manifest.json                # export level, ordered layers, original visibility/strengths
  report.json                  # topology hashes, mismatch/nonlinearity tests, warnings
```

All high-res OBJ files MUST have the same count **and ordered vertex indices AND ordered polygon corners/faces**. Merely matching vertex count is insufficient. Use identical coordinate transforms, axes and units for every file. Preserve source-space coordinates until the export transform is applied consistently.

### Shape sampling contract

Let `B` = evaluated highest-level mesh with **all geometry layers muted**. For geometry layer `i`, let `S_i` = evaluated highest-level mesh with **only layer i visible**, strength **1.0**, all other layers muted. The target point offset is `D_i = S_i - B`.

At export:
1. Require exactly one supported Multires modifier and a valid, committed Sculpt Layer state. Make a state snapshot (active layer, REC, visibility, strength, order, sculpt/view/render levels, mode).
2. Obtain consistent evaluated geometry at maximum level, with the correct native CCG/layer commit path. Do not blindly assume generic OBJ export evaluates Multires in Sculpt Mode.
3. Capture `B`, then one `S_i` per layer, then the actual composed result `F`, **without permanently changing source mesh data**.
4. Check exact topology fingerprint across snapshots, finite float coordinates, count/face order, and bounded file sizes.
5. Test additivity: `F_test = B + sum_i((visible_i ? strength_i : 0) * D_i)`; measure max/RMS error against `F`, normalize against bounding-box diagonal. If outside tolerance, warn or fail rather than pretending all layer combinations will round-trip accurately. The CCG multilayer/pose evaluation may not be purely additive in arbitrary cases.
6. Restore every source property even on exception (try/finally), including active layer and REC. Record a manifest only if all files pass integrity checks. Write to a temporary folder first, then publish atomically if feasible.

## ZBrush manual import recipe (first milestone)

1. Import `baseline_high.obj` as PolyMesh3D. Inspect visible shape and vertex count.
2. **Optional / experimental:** on a disposable tool, use `Geometry > Reconstruct Subdiv` until no more levels can be recovered. Verify top-level topology/point order survives by exporting it and comparing against the baseline. If it does not, use the **high-resolution-only** fallback, which still supports separate 3D Layers.
3. Stay at the **highest** level. Create a new Layer in `Tool > Layers`; REC turns on.
4. Ensure other transferred geometry layers are **hidden (intensity zero / visibility off)**. Run `Tool > Import` for `layer_001_*.obj`, with matching vertex count and order. End REC at the highest level. Rename this layer.
5. Repeat for every `layer_NNN_*.obj`, maintaining all previously imported layers hidden while importing the **absolute single-layer variant**. Otherwise prior visible layers contaminate the newly recorded delta.
6. Restore each layer's originally requested intensity and visibility from `manifest.json`. Compare against `preview_composed.obj`. Save an actual .ZTL/.ZPR in ZBrush; Blender cannot write these proprietary native formats itself.

If OBJ point identity changes after reconstruction, do not import delta snapshots until the mapping has been proven. Alternatives under R&D: reorder Blender sampled coordinates to match a ZBrush-generated high-res mesh with a topology correspondence table, or transfer through a projection step on a dedicated ZBrush layer (lossy).

## Suggested test fixtures and pass criteria

- L0 quad base -> Multires L2/L3; two named geometry layers (low-frequency deformation and high-frequency detail); strengths 0.35 and 1.0, one initially hidden.
- Confirm individual toggling, full/partial/negative intensity, reorder, save/reopen, and no change to original .blend.
- Test seams, extraordinary vertices, boundaries, creases, mixed quads/tris, and moderate pose changes separately. Reject unsupported topology-changing modifier stacks.
- Check `baseline_high.obj` vs each `S_i` polygon-order hash; compare Blender preview with ZBrush-composed result after normalization. Initial goal: geometry residual <= 1e-4 of object bounding-box diagonal; tighten after measuring OBJ numeric precision and ZBrush scale/import tolerances.
- Treat successfully reconstructed subdivisions as an additional reported capability (number of recovered levels and validated point-order equivalence), never an assumption.

## Implementation checklist

- [ ] Identify exact RNA/API/source hooks for active layer, visibility, strength, REC, and native Multires CCG evaluation in **current integrated branch**; the Python API is not assumed to expose the complete native layer data.
- [ ] Make non-destructive geometry snapshot engine + topology fingerprint test on a tiny fixture.
- [ ] Write OBJ variants, manifest, source state restoration, and additive composition validation.
- [ ] Test ZBrush REC + OBJ import with two layers and full/partial visibility.
- [ ] Experiment with Reconstruct Subdiv separately and verify highest-level point order.
- [ ] Optional second milestone: build a ZScript importer only after manual recipe is validated on the target ZBrush version.
- [ ] Include errors/warnings in the existing Multires UI; do not add an unrelated sidebar.
