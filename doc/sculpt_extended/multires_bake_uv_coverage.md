# Normal / Vector CPU bake: UV coverage and stable pixel ownership

Parent: `9e2d5da791d95a45694ad3cf53b019f549f69776`, on `bake/sampling-quality`.
Starts from the existing CPU 4x4 sampler and retains the duplicate-UV-vertex fixes.
No GPU, Edit/Undo or Conform code is changed.

## Reproduced problem

The existing Normal/Vector rasterizer chooses texels using their center first.
Only then does flush_pixel test 4x4 sub-samples inside that triangle. For a UV
triangle occupying the bottom-left corner of a pixel, the sample at (0.125,0.125)
can be inside while the center at (0.5,0.5) is outside. The entire texel is then
omitted despite a valid high-resolution sample.

The prior path also checks/writes its mask without synchronization while faces
run concurrently. The winning triangle for overlaps and shared texels can change
with worker arrival order.

## Isolated candidate

- Conservatively visit texels across each row's sample band, using double
  intermediates and bounds clipped before conversion to int. Visit each candidate
  once, avoiding a full axis-aligned bounding-box scan or 16 rasterizer passes.
- Keep the existing fixed sample lattice and shading methods. Shade a pixel only
  if at least one sub-sample or its center lies in the chosen triangle.
- Keep the center reference for interior pixels. If the center is outside, use
  the mean barycentric coordinate of accepted samples for an interior TBN
  reference; do not extrapolate the normal/tangent attributes outside the island.
- Prefer a triangle containing the center, then use a stable triangle index
  derived from base corners or subdivided face order. The existing hashed locks
  protect owner selection and the complete image write. A lower-ranked owner can
  replace a previous one, yielding the same final result for the same mesh.
- Retain Normal HQ's object-space normal accumulation and Vector HQ's object-space
  displacement accumulation, center-preservation and tangent channel convention.
- `BLENDER_MULTIRES_BAKE_UV_COVERAGE=0` selects the prior Normal/Vector raster path
  for comparisons at the same 4x4 setting. Restart Blender after changing it.

True overlaps use a deterministic winner, not blended geometry. Reindexing mesh
faces may change that winner. This does not solve genuine UV ambiguity.

## Memory and performance

The temporary uint64 owner buffer costs 8 bytes per pixel: 128 MiB at 4096x4096
or 512 MiB at 8192x8192, only for a Normal/Vector HQ tile. It is released immediately
after that tile's rasterization, before processing the next tile. No new dense
per-sample buffer is added. A/B timings and memory measurements on Windows are
still required: locks include sampling and writes and can cause contention.

The scalar coverage/center accumulator, padding implementations and
create_highres_mesh() are not altered. The diagnostic 1x1 path retains its prior
behavior and is not covered by the new deterministic owner mechanism.

## Validation status

Standalone C++17 harness extracts the actual rasterizer, pixel-owner and flush
functions. It mocks the barycentric service (matching Blender's weight convention),
shading, ImBuf and spin-lock services. It does not use native CCG/TBN shaders.

- Existing branch reproduces the omitted corner texel.
- Candidate: 46,048 coverage comparisons with an independent double-precision
  oracle (near-edge cases within 1e-5 barycentric distance excluded from the random
  oracle comparison; exact seam cases are tested separately).
- 720 winding-reversed triangles have matching coverage.
- 24 random overlap orderings with eight workers give bit-identical mock float
  images and owners for each ordering.
- Explicit center-priority, tiny-center fallback, empty subpixel candidates,
  shared diagonal, distant/non-finite UVs and collinear tests pass.
- Passed g++ -Wall -Wextra -Werror and AddressSanitizer/UndefinedBehaviorSanitizer.
  LeakSanitizer is disabled due executor instrumentation limitations.
- Prior 27 duplicate-vertex guard tests still pass.

Blender is NOT compiled here and native bake images are NOT validated. Keep draft.

The native smoke script uses the fork's BakeSettings RNA (use_multires, type,
displacement_space, use_lores_mesh) and object.bake_image exec path, verified in
rna_scene.cc/object_bake.cc. Only Python syntax was checked here. It starts from
a fresh background scene and checks five Normal/Vector coverage cases ON/OFF.

## Remaining quality work

This candidate still shades a pixel from one selected triangle: it does not
combine all neighboring-triangle samples like scalar coverage. Triangle-crossing
footprints, mirrored seams and mip/padding transitions require native image
tests and a later island-aware integration design. Do not advertise complete
seam elimination or full scalar/vector parity.

Scalar overlaps remain arrival-dependent and rejected samples can still affect
normalization bounds. Those are separate follow-ups.

Before adaptive sampling, check the full bake-to-render round trip:
target low surface, triangulation and TBN used by the renderer must match; reapply
displacement to that surface and measure position error and normal angle against
the original high-resolution sculpt. A cleaner map alone does not prove a more
accurate reconstruction.

## Native test sequence

1. Fresh Multires scene, float image, margin 0, clear target, explicit non-overlapping
   UVs. Put a sculpt feature near a UV edge. Bake Normal and object/tangent Vector
   at 4x4 with coverage enabled, then disabled in a separate process.
2. Thin islands, exact shared diagonal, mirrored seams, stacked islands, UVs on
   UDIM borders. Inspect unpadded float pixels before comparing padded versions.
3. Repeat with 1, 2 and 8 Blender worker threads and compare float pixels exactly.
4. Check conventional interior normals and vector center displacement against
   the prior HQ result, and test both Use Low Resolution Mesh settings.
5. Compare Normal/Vector reapplication to the source sculpt, not just maps.
6. Measure bake time and peak memory at 2K/4K/8K before merging.

