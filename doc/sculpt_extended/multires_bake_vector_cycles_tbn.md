# Vector Tangent: Cycles frame candidate

Native pre-patch audit on Blender 5.2.1 LTS: 120 finite nonzero displacement
probes, matched Object and Tangent texels on original smooth triangles with
regular/mirrored UVs. Source frame hash matches the earlier test scene.
Independent position-error recalculation agrees with uploaded report to 1.3e-10.

Current Cycles-decoded Vector Tangent vs Vector Object:
- Direction RMS 1.448308 degrees, maximum 2.751788 degrees.
- Relative vector error RMS 2.679712 percent.
- Amplitude error RMS 0.914050 percent.
- Object position error RMS 0.004665195 Blender units.
Both UV orientations show similar mismatch. Object displacement length ranges
0.11657 to 0.21713 in this scene. These are space-conversion diagnostics, not
absolute high-surface reconstruction errors.

The analytical exact-inverse candidate round-trips at 1.18e-8 units RMS, but that
is algebraic, not a native production test. The native pre-patch report is retained
in validation/native-vector-frame-before.json. It does not include a binary hash.

## Small production change

Add a Vector-only helper matching kernel/svm/displace.h: normalized object-space
N, raw interpolated T, normalized cross(N,T) multiplied by interpolated sign.
The helper returns the inverse T,B,N matrix; existing callers still pack channels
T,N,B. Do not normalize displacement magnitude. Change the two Vector Tangent
conversion call sites (single-sample and HQ finalize). Preserve degenerate-frame
fallback, absent-tangent identity behavior and previous route opt-out.

BLENDER_MULTIRES_BAKE_VECTOR_CYCLES_TBN=0 selects the prior orthonormal HQ frame;
default is enabled and the flag is read once per process. Object and scalar
bakes, Normal HQ fix, UV ownership, sampling lattice, center/partial reference,
padding, high-resolution allocation and GPU code are unchanged.

No speedup claimed. Existing tangent maps are not rewritten; this changes newly
baked Tangent maps. Other applications' tangent conventions may differ; this is
specifically a Cycles-compatible bake conversion. Main stays frozen.

## Minimal validation, per user request

Here: source inspection, Python syntax and patch apply/reverse against the exact
repository source at 97fb0fc3. No native Blender compilation or new broad test
suite. User: incremental INSTALL on the existing Windows build, then the same
two 256x256 bakes/120 probes, with corrected flag enabled. Report is named
native-vector-frame-candidate.json. Review its actual decoded errors before
claiming success. Native rendering, seams, split/custom normals, transformed
objects and full reconstructed geometry remain outside this test.
