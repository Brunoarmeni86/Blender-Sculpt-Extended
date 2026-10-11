# CPU Multires sampling audit, 2026-10-11

Reference: `83d81b2dfc2af8ba7763d33fce212dbaa9b5dc49`.
GitHub comparison confirmed `bake/sampling-quality` initially identical to this
baseline. No GPU changes are proposed.

## Isolated correction

Three rasterizer duplicate-vertex guards compared the X coordinate of vertex
0 with vertex 1 while comparing the Y coordinate of vertex 0 with vertex 2.
This rejects a valid UV triangle such as `(0,0), (0,1), (1,0)`. Replace that
mixed comparison with the actual vertex-0/vertex-2 duplicate check in the
center-reference, coverage, and legacy/Normal/Vector rasterizers.

Validation: standalone regression extracts and compiles the actual three
guards. All six permutations of the valid right triangle and all three
duplicate-vertex pairs pass: 27 checks. Unmodified baseline fails the valid
right-triangle case. This validates the guard correction only; Blender has
not been compiled and bake images have not been tested for this change.

## Current implementation and subsequent work

- The default quality setting is 4x4, with diagnostic 1x1, 2x2 and 8x8.
- Scalar displacement uses per-sub-sample rasterization with a 16-bit coverage
  mask and hashed spin locks for 2x2/4x4. Fully covered pixels use an exact
  center sample; partial pixels average their covered samples.
- Normal and Vector Displacement already support supersampling, but do not
  use the scalar cross-triangle coverage accumulator. Their rasterizer first
  selects pixels by center coverage, then averages samples contained in the
  selected triangle. Samples from neighboring triangles are not combined.
  UV edges whose pixel centers are outside can therefore miss partial pixel
  coverage. Test this before extending their coverage machinery.
- Normal HQ averages directions in object space and converts once with an
  orthonormalized center TBN. Vector HQ likewise accumulates object-space
  displacement and converts once; it preserves a center sample when all
  samples lie in one triangle. Preserve those semantics during an extension.
- Pixel ownership in the Normal/Vector path uses an unprotected mask
  check/write while faces are processed in parallel. In scalar coverage,
  locks serialize first-arriving sample ownership but do not establish a
  deterministic ordering for true UV overlaps. Overlap tests and an explicit
  ownership policy are needed; these are not fixed by the guard correction.
- Scalar normalization bounds are collected during sample evaluation before
  ownership is accepted. Rejected overlapping samples can influence the
  normalization range. Keep this distinct from coverage tests.
- Padding already supports Extend and Adjacent Faces. Their behavior should
  be verified on seams, mirrored UVs, and UDIM tile boundaries before adding
  another padding scheme.
- With Use Low Resolution Mesh off, displacement/vector displacement still
  create a temporary high-resolution mesh and per-corner grid mapping. Its
  memory reduction is a separate optimization task.
- The sampling introduction says Vector Displacement is unchanged, but its
  actual class enables HQ; the comment is stale.

Suggested integration scenes: right-triangle UVs; triangles sharing a pixel;
thin/subpixel islands; mirrored UV seam; stacked islands; UDIM edge; tangent
and object-space vector displacement. Repeat with varying worker counts and
compare float pixels before padding, then after padding. Adaptive sampling
should follow correctness and repeatability tests.
