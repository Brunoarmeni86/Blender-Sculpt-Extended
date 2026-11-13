# Blender Sculpt Extended — Simple User Guide

**For the stable Windows 5.2.3 build released on 4 November 2026**

Welcome! This guide is for artists who want to try the new sculpting tools without reading the development notes.

**[Download the stable build](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261104)** · **[Report a problem](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues)** · **[Full feature overview](../../README.md)**

> **Please read first:** Sculpt Extended is an **experimental, unofficial Blender build**. Work on a **copy** of important projects, especially before remeshing, baking layers, or changing topology. The steps below describe the **4 November stable checkpoint**, not every experimental feature in the source code. Some tool labels in this build are still Italian; the original button text is included where helpful.

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

## 4. Pose, Transform Blend and the shared pivot

The latest stable build improves smooth transitions when moving, rotating or scaling parts of a sculpt.

### Pose Segment Blend

1. Choose the **Pose** brush in Sculpt Mode.
2. Work at a lower Multires Sculpt level for larger adjustments.
3. Find **Segment Blend** in the Pose brush settings and adjust it to smooth the transition between segments.
4. Return to your highest Sculpt level to check the detail.

### Shared transform pivot

Some pivot controls in the stable build use Italian labels:

1. Press **N → Tool → Snap e pivot Sculpt**.
2. Enable **Mostra pivot trasformazioni** (*Show Transform Pivot*).
3. Keep **Condividi con simmetria** enabled to share the symmetry reference; disable it if you need a separate transform pivot.
4. Choose **Posiziona pivot con snap** (*Place Pivot with Snap*) to reposition it. **G** moves, **X/Y/Z** constrains the axis, **Ctrl** snaps, and **Esc** cancels.
5. Use sculpt **Move / Rotate / Scale** with the pivot. Adjust **Transform Blend** / falloff in the relevant tool settings for smoother transitions.

You can also use **Usa riferimento come pivot** (*Use Reference as Pivot*) or **Pivot al cursore 3D** (*Pivot at 3D Cursor*).

**Note:** continuous symmetry while sculpting an *asymmetrically posed* character is still under development. The current reference/pivot controls do not make every stroke pose-symmetric.

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
2. Choose **Attiva pennello Face Sets HR** (*Activate Face Sets HR Brush*) or use the **Box / Lasso / Line** tools.
3. Select a **Face Set ID** to hide, isolate, show, grow/shrink or convert to a mask.

These tools have specific restrictions. The panel currently expects **a single Multires modifier with no other modifiers** and can reject very large or incompatible meshes. **Extract Face Set HR** is experimental, needs Sculpt level **L1 or higher**, and may require an opt-in setting.

For ordinary mask-protected shaping, paint a mask over the area you want protected **before** executing the shaping operation.

## 7. Symmetry reference and mask-protected operations

1. In Sculpt or Edit Mode, choose the **Riferimento simmetria** (*Symmetry Reference*) tool when available.
2. Position it with **G / R / S**, optional **X/Y/Z**, **Ctrl** to snap and **Esc** to cancel.
3. For symmetry operations that should preserve a masked part, use **Solo forma — maschera protetta** (*Shape Only — Protected Mask*) and select **Auto**, **Topology** or **Surface** as appropriate.
4. If using the supported masked **Voxel Remesh** workflow, look for **Proteggi zona mascherata** (*Protect Masked Region*). Inspect its border and the result carefully.

**Important:** remeshing changes topology and can be expensive. Do not expect arbitrary topology changes to preserve all Multires layer identities automatically. Save a copy before remeshing.

## 8. Other tools worth trying

| Tool | Where / how | When it helps |
| --- | --- | --- |
| **Draw Transport** | **N → Multires → Brush Options → Activate Draw Transport** | Work with supported anchored Draw detail; **R** rotates, **S** scales, **SS** adjusts strength in that interaction |
| **Detail Transport Copy V01** *(experimental)* | Open **Detail Transport Copy V01**; choose source/target → **Crea anteprima separata** → inspect → **Conferma in nuovo layer** | Transfer/projection experiments between aligned meshes |
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
