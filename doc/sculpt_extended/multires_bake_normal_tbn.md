# Normal HQ: preserve the renderer's interpolated tangent frame

Parent: tested CPU UV coverage checkpoint 18931a2. Main remains 83d81b2.

The Normal HQ finalizer previously inverted an orthonormalized frame. Cycles
`intern/cycles/kernel/svm/tex_coord.h::svm_node_normal_map` instead reconstructs
with interpolated, unnormalized N and T, B = sign * cross(N, T), then normalizes
the resulting direction. Altering that frame at bake time introduces a direction
error on curved triangles even with perfectly sampled high-resolution normals.

This change uses the existing raw TBN inverse in the Normal HQ finalizer, as the
legacy 1x1 Normal path already does. Object-space accumulation, reference pixel,
coverage, RGB encoding and neutral fallback are unchanged. Vector Displacement
continues using its existing HQ frame; its shader and magnitude convention need
a separate audit. Scalar displacement, padding, high-resolution mesh creation,
GPU code, sample count and temporary memory are unchanged.

Diagnostic opt-out: `BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN=0` selects the
previous HQ conversion. The flag is read once per process. Default is enabled.

## Validation

The extracted actual finalizer, with portable math services, passes 216 cases
covering curvature, twist, mirrored handedness, encode/decode and fallbacks.
Maximum angular reconstruction error: new 0.00000855 degrees; old 2.73419409
degrees. Legacy opt-out RGB is byte-identical to the prior finalizer. These are
synthetic tests, not native Blender or Cycles renders. g++ warnings-as-errors,
ASan and UBSan pass (LeakSanitizer disabled because this executor cannot inspect
process threads). The existing 46048 coverage comparisons, 720 winding reversals,
24 threaded overlap permutations and 27 raster guards also pass.

The new native script is syntax-checked but has NOT run here; Blender and
PowerShell are unavailable. It builds an ellipsoidal, smooth triangular mesh,
regular/mirrored independent UV islands, and Multires level 3. Three fresh
processes bake legacy 1x1, previous 4x4 and corrected 4x4. At 120 interior pixels
it decodes using native low-mesh tangent/normal data and the Cycles equation.
It reports angles relative to the center-sampled 1x1 map. HQ filters a footprint,
so this reference is a diagnostic, not an exact expected HQ result. A changed
output is required; a lower error is reported, not assumed. The runner also
records the executable SHA256. A failed probe must be investigated, not treated
as a validated feature.

Pending: native build/test, actual Cycles render reconstruction, split/custom
corner normals, quad triangulation agreement, texture filtering, transformed
objects and full seams. The underlying baker still uses vertex normals in some
raster frame builders; this patch does not claim to solve that mismatch.

## Windows

Keep the already-tested UV coverage patch installed. Apply this additional
patch using APPLY-BAKE-NORMAL-TBN.ps1, reusing build_windows_x64_shareable.
It checks applicability, backs up multires_bake.cc and builds INSTALL without
reconfiguring. -Revert removes only this Normal conversion patch.

TEST-BAKE-NORMAL-TBN.ps1 writes three native-normal-*.json files plus
native-normal-comparison.json and native-normal-binary.txt. Close Blender before
installation. Send the comparison and reports back for review before merging.

## Native probe correction (2026-10-12)

The first Windows run stopped in the 1x reference with missing coverage at face 0,
pixel (13,36). The probe created TBNProbe without activating it, while the
icosphere already had its generated UV map. Repository RNA confirms new UV maps
only become active when no map was active; the baker reads active_uv_map_name.
The corrected probe disables primitive UV generation, explicitly activates
TBNProbe for editing/rendering and checks it before baking. No C++ changes or
rebuild required. Native precision validation remains pending.

## Native diagnostic result

The corrected Windows probe now passes on Blender 5.2.1 LTS: 120 matching
finite covered samples. RMS relative to the 1x center reference improves from
1.083002470 to 0.285127835 degrees, with 114/120 closer samples and improvements
in both regular and mirrored islands. See validation/multires_bake_native_normal_tbn_20261012.md
for executable hash, retained reports and limits. Actual Cycles render validation
and the other listed compatibility cases remain pending.
