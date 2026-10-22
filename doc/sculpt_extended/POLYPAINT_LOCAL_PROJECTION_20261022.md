# Polypaint V04 local projection — 22 October 2026

The active V04 brush path previously projected every pixel of each affected HR master grid at every dab, even when only a small portion changed. It also used bilinear map lookups when painting directly at the master level, where the projection is an indexed delta write.

Changes:

- Initialize a dense staged grid with one contiguous copy, preserving full source-color validation. Implicit grids still initialize from the base attribute.
- At matching paint/master resolution, apply only the indexed deltas.
- At lower paint resolutions, evaluate the original interpolation inside a conservative support rectangle, including the neighboring coarse cells required by bilinear interpolation. Samples outside it retain their original bits.
- Look up the four coarse deltas once per projected pixel, instead of repeating those lookups per channel. Preserve the original arithmetic order within each channel.

All affected grids remain staged before publication. Alpha preservation for Blur/Smear, mask/hide protection, seam grouping, Wet Mix, budget checks and Undo routing remain in place. Full-grid allocation/copy and validation are still required by this implementation; this is not a new sparse storage format or a claim of lower peak RAM. Global validation and serial brush-factor collection are not changed. V3 parallel/prepared flags do not become active in the V04 path through this patch.

Verification: Release build succeeds. The standalone C++ numerical comparison matches the original projection bit for bit across matching and coarse/master resolutions, partial/full protection, 3/4 channels, boundary samples and signed zero. It also verifies unchanged reference output outside the conservative support rectangle. The check runs without Blender scenes or performance benchmarks.

Manual painting, Blur/Smear, coarse-level color-detail preservation, Undo/Redo and speed measurements are left to the user. No measured speedup or complete manual acceptance is claimed yet. Keep the previous binary available through the existing historical ZIPs.
