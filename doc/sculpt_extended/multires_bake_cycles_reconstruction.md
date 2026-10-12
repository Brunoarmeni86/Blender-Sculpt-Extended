# Cycles reconstruction diagnostic kit

Based on the tested Normal HQ/UV coverage branch b5c0f988. No production changes.

The kit reuses the native ellipsoid/TBNProbe scene that passed on Windows.
Two fresh CPU Cycles processes select previous and corrected Normal HQ conversion
with the same CPU 4x4 sampling. Each process exports Normal, Vector Object,
Vector Tangent and scalar maps, and renders:

- Actual evaluated Multires level 3, shading normals encoded as emission RGB.
- Original low mesh with tangent Normal Map, Original Base.
- Simple subdivision level 4 + Vector Object displacement + Normal Map.
- The same reconstruction using Vector Tangent displacement.

Outputs: linear RGBA EXR, float arrays for comparison, PNG previews, scene .blend,
per-mode JSON, final comparison JSON and executable hash. Normal maps are padded
by eight pixels; regular/mirrored UV islands are retained. All render pixels must
be finite. The comparison checks identical reference renders and excludes
antialiased silhouettes using a common alpha mask. Shared-edge bilinear probes
also report decoded normal jumps, Vector Object jumps and normalized scalar jumps.
They are diagnostics: no fabricated pass threshold or zero-seam assumption.

## Limits and interpretation

This kit has NOT run in native Blender or PowerShell here. Python syntax and
standalone comparison plumbing are checked. BakeSettings, Normal Map base=ORIGINAL,
material displacement_method and Vector Displacement conventions were inspected
in the actual repository RNA and Cycles shader code.

Simple subdivision can change tangent frames relative to the original low-mesh
bake frame. Texture filtering, partial texel ownership and padding may contribute
additional error. Screen-space comparison against high geometry is not an exact
surface-point correspondence when reconstruction changes positions. Read the
normal-only and paired cases separately; improvement in one does not prove the
other. The scene has Multires curvature but no user-sculpted detail.

Normal Map reconstructs raw interpolated N/T and sign*cross(N,T). Cycles Vector
Tangent uses its own convention: normalized bitangent and channel order T,N,B.
The baker currently orthonormalizes its Vector frame. This is an audit target;
the new test does not change Vector conversion based on the Normal fix.

Scalar bake normalization is H=.5+d/(2*A), A=max(abs(min),abs(max)). Native bake
RNA does not expose A. The scalar map is therefore exported for inspection and
normalized seam continuity only; physical scalar reconstruction is NOT claimed.
A future change should expose accurate bake scale rather than infer it from
texture extrema or guess it.

## Windows

Extract Bake-Cycles-Reconstruction and execute TEST-BAKE-CYCLES-RECONSTRUCTION.ps1
using the existing compiled Blender. No source patch, install or CMake invocation.
The default engine is CPU Cycles at 256x256, 16 samples, fixed seed, no denoising
or adaptive sampling. Two processes render four views each. Reports are saved
under a timestamped cycles-test folder. Review native errors before claiming pass.

## Recover existing renders after strict reference check (2026-10-12)

The native run completed rendering but comparison stopped because reference
RGBA differed by more than 1e-6. The comparator now reports the full reference
drift and its status, rather than hiding it or assuming it is numerical noise.
It audits saved HighReference mesh/camera fingerprints when running in Blender,
uses both reference alpha masks, compares each candidate to its own reference,
and supplies both cross-control comparisons. A review status is not a pass.
No new render, bake, source modification or build is required for recovery.
