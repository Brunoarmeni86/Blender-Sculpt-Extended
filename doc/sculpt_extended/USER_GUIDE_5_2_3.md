# Blender Sculpt Extended — Simple User Guide

**For the stable Windows 5.2.3 build released on 4 November 2026**

Welcome! This guide is for artists who want to try the new sculpting tools without reading the development notes.

**[Download the stable build](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261104)** · **[Report a problem](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues)** · **[Full feature overview](../../README.md)**

> **Please read first:** Sculpt Extended is an **experimental, unofficial Blender build**. Work on a **copy** of important projects, especially before remeshing, baking layers, or changing topology. This guide covers the **4 November stable checkpoint**, not every experimental feature in the source. All explanations are in English; when a button still has an Italian name in this release, its exact on-screen label is shown in `code` next to the English meaning.

## 1. Install and find the tools

1. Open the [stable release page](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261104).
2. Download either **Windows x64 Standard** or **Windows x64 Portable**. Portable keeps its configuration next to the application; Standard uses the usual Blender user configuration.
3. Extract the **whole ZIP** to a separate folder. Do not mix files from different Blender builds.
4. Start **`blender.exe`**. You can keep your normal Blender installation alongside it.
5. Select a mesh, enter **Sculpt Mode**, press **N** in the 3D Viewport and open the **Multires** tab.

Many tools are under **N → Multires**. Pivot and symmetry-reference controls are under **N → Tool**. The normal **Modifier Properties** and brush settings are still used too.

**If a panel is missing:** make sure a mesh is selected, that you are in the correct mode, and that it has **exactly one Multiresolution modifier** where required. Some advanced tools only appear with compatible geometry or feature settings.

## 2. Your first Sculpt Layer (about five minutes)

Sculpt Layers let you make a separate sculpting pass and adjust it later without permanently changing the base.

1. Select a mesh and add a **Multiresolution** modifier in **Modifier Properties**.
2. Press **Subdivide** a few times. Start small (two or three levels) while learning.
3. Enter **Sculpt Mode** and open **N → Multires → Sculpt Layers**.
4. Press **+** to add a layer. Select it with the **radio button** beside its name; rename it, for example, `Skin folds`.
5. Make a small test stroke with **Draw** or **Clay**. If recording is not active, enable **REC / recording** in the available Sculpt Layer controls. Recording requires a visible editable layer with non-zero Strength.
6. Change the layer's **Strength** to make the effect weaker or stronger; use the **eye icon** to hide/show it.
7. Add another layer for a separate sculpting pass. Use the arrows, duplicate and group controls to organize layers.

**Tip for very dense layers:** the small **clock icon** beside Strength opens a deferred editor. Enter a value and confirm with **OK** to apply it once, instead of rebuilding the result for every movement of the ordinary Strength slider. Use the normal slider when you want continuous feedback.

**Useful buttons in this panel**

| Button | What it does |
| --- | --- |
| **+ / −** | Add or remove a Sculpt Layer |
| **Radio button** | Choose the active layer |
| **Strength** | Change how much that layer contributes |
| **Eye** | Show or hide the layer |
| **Duplicate / arrows / Group** | Organize sculpting passes |
| **Commit Pending Changes** | Finish recording pending geometry before certain operations |
| **Create Hidden Copy** | Make a hidden copy/snapshot |
| **Bake Visible into Base…** | Bake visible sculpted contributions into the base on supported Multires setups |

**Bake is not the same as hiding or merging:** baking changes the stored result. The dedicated **Bake Visible into Base…** command is designed to preserve hidden-layer data; save a separate file first and inspect the result before continuing. **Merge Visible into Base** is a different command available in **Object Mode**.

## 3. Sculpt at a lower Multires level without losing fine details

This is one of the important improvements in the 4 November build.

1. Sculpt some fine details at a higher **Sculpt** level.
2. Lower the **Sculpt** level in **N → Multires** or the Multiresolution modifier.
3. Use a supported brush such as **Smooth** or **Pose** for broader changes.
4. Return to the original higher level and check the small details.

The new Pose/Multires corrections help avoid the **spikes** that could appear when posing at a lower level and returning to the highest level. The deformation is transported in object space before being converted back to the fine grid's local frame.

**Keep in mind:** this improves supported workflows, but it is not a guarantee that every modifier, brush and extreme deformation is artifact-free. If the shape looks wrong, stop and report the exact steps rather than repeatedly baking it.

**Preserve Other Layers:** for supported brushes, see **N → Multires → Brush Options**. Use this option when you want to reduce unwanted changes to other layer contributions. It is not a universal guarantee of identical visible detail after a very large deformation.

## 4. Pose, Transform Blend and the symmetry reference/pivot

The latest stable build improves transitions when posing or transforming a sculpt. It also provides a **movable symmetry reference** and an optional **shared transform pivot**. These are related tools, but they have different purposes.

### First, understand the difference

| Control | What it does | When it is useful |
| --- | --- | --- |
| **Symmetry Reference** | Sets the **location and orientation of the symmetry planes** used by supported Sculpt Extended tools. | Your model is offset from the object origin, or its anatomical symmetry plane is tilted. |
| **Sculpt Transform Pivot** | Sets the **center for supported Move / Rotate / Scale transforms** in Sculpt Mode. | You want to transform a jaw, shoulder or other region around a particular point. |
| **Share with Symmetry** | Links the sculpt transform pivot to the symmetry reference. | You want the transform pivot and the symmetry reference to remain aligned. |

**Important:** placing the symmetry reference does **not** automatically activate symmetry for every Blender brush. Enable the required symmetry axis in the relevant tool and verify that the operation supports the custom reference.

### A. Place and orient the symmetry reference

1. Enter **Sculpt Mode** (or **Edit Mode** where the reference tool is available).
2. Select the **Symmetry Reference** workspace tool. In the 4 November build its on-screen name may be `Riferimento simmetria`.
3. Press **G** to move the reference to the **centerline of the part you want to mirror** — for example, between the eyes or at the center of the chest.
4. Press **X**, **Y** or **Z** to constrain movement. Hold **Ctrl** for snapping when appropriate; numeric entry is also supported.
5. Use **R** to rotate the reference if the intended symmetry plane is not aligned with the model's object axes. This changes the reference **orientation**, not the shape of the mesh.
6. Use **S** to make the **displayed reference planes** larger or smaller. **S changes only the gizmo/plane display size — it does not scale the sculpt or extend the mirror's influence.**
7. Confirm the transform, or press **Esc** to cancel. Moving the reference itself does not deform the mesh.

If the reference is locked or attached to a target, unlock or detach it before repositioning it.

**Example — an off-center creature head:** suppose the head is not centered on the object's origin. Move the symmetry reference to the center of the face instead of moving the entire object. If the head is tilted, rotate the reference to match its anatomical midline. Then test a **supported** mirror operation and its selected axis on a small, reversible edit.

### B. Link the Sculpt pivot to the symmetry reference

Use this when the **symmetry center** and **transform center** should coincide.

1. Position the **Symmetry Reference** as described above.
2. Enter **Sculpt Mode** and open **N → Tool → Snap and Sculpt Pivot**. In this release the panel may be called `Snap e pivot Sculpt`.
3. Enable **Show Transform Pivot** (`Mostra pivot trasformazioni`) to display its gizmo.
4. Enable **Share with Symmetry** (`Condividi con simmetria`).
5. If needed, use **Use Reference as Pivot** (`Usa riferimento come pivot`) to place the pivot at the reference.
6. To move the linked point, choose **Place Pivot with Snap** (`Posiziona pivot con snap`) or drag one of the pivot's axis handles. Use **X/Y/Z**, **Ctrl** snapping and **Esc** to cancel.
7. Select a supported Sculpt **Move**, **Rotate** or **Scale** tool. These use the transform pivot; **Transform Blend** and the brush falloff determine how surrounding geometry is affected.

**Key behavior:** when **Share with Symmetry** is enabled, relocating the shared pivot **can also relocate the symmetry reference**. That is intentional. If you want the symmetry plane to stay fixed, **unlink the pivot first**.

Linking the pivot does not mean that every Move/Rotate/Scale stroke automatically mirrors to the other side. Mirroring depends on the selected symmetry mode and its support in the particular tool.

### C. Keep symmetry fixed while using a different transform pivot

For joint or facial work, independent control may be preferable:

1. Set the **Symmetry Reference** at the model's anatomical centerline.
2. Open **N → Tool → Snap and Sculpt Pivot** and **disable Share with Symmetry**.
3. Move only the **Sculpt Transform Pivot** with **Place Pivot with Snap**, or use **Pivot at 3D Cursor** (`Pivot al cursore 3D`).
4. Transform your chosen sculpt region around its new pivot. The symmetry reference stays in place.

**Example — creature jaw:** leave the symmetry reference between the eyes, but move the independent transform pivot to the jaw hinge. The jaw can now be rotated about its hinge without shifting the face's symmetry plane.

### D. Pose Segment Blend

1. Choose the **Pose** brush in Sculpt Mode.
2. When appropriate, work at a lower Multires Sculpt level for broad adjustments.
3. Find **Segment Blend** in the Pose brush settings and adjust it to soften the transition between pose segments.
4. Return to the highest Sculpt level and check the preserved details.

**Current limit:** the symmetry reference and shared pivot are **not yet a complete pose-aware symmetry system**. Continuous mirroring of every stroke across differently posed limbs is still under development.

## 5. Paint color directly on Multires (Polypaint HR)

1. Select a compatible Multires mesh and enter **Sculpt Mode**.
2. Open **N → Multires → Polypaint Attributes** (some color tools also appear as **Color Attribute HR**).
3. Click **Add** to create a color attribute, or select an existing one.
4. Use the supported **Paint**, **Blur** and **Smear** controls in Sculpt Mode to add or soften color.
5. If you want to display the color in a material, use the corresponding **Color Attribute** in the shader.
6. For an image, choose a UV map and use **Polypaint UV Bake V01**. Save the resulting image to disk.

The V01 UV bake handles **one UV tile at a time**; it is **not** an automatic multi-UDIM export. Different named color attributes can be selected, but this is not yet a Photoshop-style color-layer compositor.

**Tip:** large color-master and Undo operations can be slow on very dense meshes. Save a backup before deleting or replacing color attributes.

## 6. Work with high-resolution Face Sets and masks

Face Sets HR helps select, isolate or mask details on a compatible Multires mesh.

1. Enter **Sculpt Mode** and open **N → Multires → Face Sets HR**.
2. Choose **Activate Face Sets HR Brush** (shown as `Attiva pennello Face Sets HR` in this release), or use the **Box / Lasso / Line** tools.
3. Select a **Face Set ID** to hide, isolate, show, grow/shrink or convert to a mask.

These tools have specific restrictions. The panel currently expects **a single Multires modifier with no other modifiers** and can reject very large or incompatible meshes. **Extract Face Set HR** is experimental, needs Sculpt level **L1 or higher**, and may require an opt-in setting.

For ordinary mask-protected shaping, paint a mask over the area you want protected **before** executing the shaping operation.

## 7. Mask-protected symmetry and remesh operations

Before running a symmetry operation, position the **Symmetry Reference** and choose whether the transform pivot should share it. See [Section 4](#4-pose-transform-blend-and-the-symmetry-referencepivot) for a practical walkthrough.

To protect a specific region of the mesh:

1. Paint a **Mask** over the region you want to preserve.
2. Choose **Shape Only — Protected Mask** (shown as `Solo forma — maschera protetta` in the 4 November build).
3. Select **Auto**, **Topology** or **Surface**, according to the supported matching method.
4. Run the operation and inspect the boundary, fine details and affected side.

For the supported masked **Voxel Remesh** workflow, enable **Protect Masked Region** (`Proteggi zona mascherata`) and inspect the result carefully.

**Remember:** moving the symmetry reference or pivot is a non-destructive **tool-placement** operation. Shape Only and Remesh can actually modify geometry or topology. Very fine remesh settings may use substantial memory, and arbitrary topology changes do not guarantee that editable Multires layers remain transferable. Save a separate project copy first.

## 8. Other tools worth trying

| Tool | Where / how | When it helps |
| --- | --- | --- |
| **Draw Transport** | **N → Multires → Brush Options → Activate Draw Transport** | Work with supported anchored Draw detail; **R** rotates, **S** scales, **SS** adjusts strength in that interaction |
| **Detail Transport Copy V01** *(experimental)* | Choose source/target → **Create Separate Preview** (`Crea anteprima separata`) → inspect → **Confirm in New Layer** (`Conferma in nuovo layer`) | Transfer/projection experiments between aligned meshes |
| **Displacement → active layer** | Use the displacement bake-to-active-layer operation with a valid Multires layer selected | Make a supported displacement result editable as a Sculpt Layer |
| **Shrinkwrap → active layer** | Use the corresponding bake-to-active-layer operation | Record a supported shrinkwrap result on a layer |

For the experimental tools, start on a low-resolution **copy** and confirm the modifier, UV/mapping, mask and source/target requirements first.

## 9. Which features are *not* in this stable build?

These ideas are being researched, but are **not finished features in the 4 November release**:

- **NanoMesh** instance-painting and hybrid displacement.
- **Amplify / Repeat Last Stroke** as discussed for a future build.
- **Bmax-style Symmetry Blend** (gradual blending of two sides).
- **Poseable Symmetry** for every stroke on a posed mesh.
- **MicroDetail** beyond the existing Multires grid system.
- **Adaptive Sculpt LOD** with mixed resolutions in one object.

**About the 12 November candidate:** a newer *prerelease* experiments with mask-protected local surface remeshing. It has a **known Undo issue**: the first Undo may not restore the exact pre-remesh topology. It is **not** the stable build recommended by this guide. If you test it, use disposable copies and check its [release notes](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-candidate-20261112).

## 10. Quick troubleshooting and bug reports

**I can't find Sculpt Layers:** add exactly one Multiresolution modifier, enter Sculpt Mode, open **N → Multires**.

**The brush doesn't affect the layer:** select the correct layer, make it visible, check that Strength isn't zero and that recording is enabled. Some operations require a Commit or have restricted modifier stacks.

**Strength is slow:** try the **clock icon** for a single confirmed change instead of dragging the slider continuously on a very dense layer.

**My detail changed after Pose:** note the Sculpt level used for the stroke and the level where you first saw the problem; keep the original file and try a simpler test mesh.

**Undo or Polypaint freezes:** don't assume Blender crashed immediately; check memory usage. On very dense scenes, some global rebuilds remain expensive. Save safe checkpoints frequently.

When [opening a GitHub issue](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues), please include:

- Build name or release tag, Windows version and GPU if relevant.
- Modifier order, Sculpt level / total Multires levels and whether REC, layers or masks were active.
- The exact sequence of actions, expected result and actual result.
- Whether Undo/Redo or reopening the file changes the result.
- A small example file or screenshot if you can share one.

---

**Want more details?** Read the [full README](../../README.md) for the feature inventory and limitations, or the [4 November release notes](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261104).

Thanks for trying Sculpt Extended and sharing feedback.
