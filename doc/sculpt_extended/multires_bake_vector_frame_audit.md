# Minimal native Vector Tangent frame audit

No production changes. Continue from c044b9c1; previous Cycles reconstruction
comparison remains inconclusive. This test reduces scope to two 256x256 native
CPU bakes and 120 corresponding texels, no rendering or geometry reconstruction.

Use the existing ellipsoid/triangular UV layout, active UV explicitly selected,
regular and mirrored islands, Multires level 3 and original low cage level 0.
Bake Vector Object and Vector Tangent with the current 4x4 path. Decode current
Tangent values through the actual Cycles convention from kernel/svm/displace.h:
N is normalized in object space, T is raw interpolated tangent, B is normalized
cross(N,T) multiplied by the interpolated sign, channels T,N,B. Identity object
transform and smooth original triangular faces isolate this convention.

The baker currently orthonormalizes its Vector frame, so magnitude/direction may
change when the shader uses a different frame. Report position, relative vector,
angle and amplitude errors against the Object displacement at the same texels.
Object bake is a space-conversion control, not a sculpt reconstruction oracle.
Unequal near-corner probe weights avoid internal CCG diagonals and UV boundaries.

An analytical candidate inverses the exact Cycles matrix in Python without
normalizing the displacement. Its round-trip is algebraic, not an independent
quality test or a compiled implementation. Only the observed current-native
Tangent-vs-Object mismatch can motivate a small production patch; candidate
round-trip alone does not establish usefulness.

Checks performed here: Python syntax and repository source inspection only.
No native Blender or PowerShell execution. The user runs TEST-VECTOR-FRAME.ps1
on the existing build and returns native-vector-frame.json and binary-sha256.txt.
No CMake, INSTALL, source patch or merge. Main remains frozen; scope excludes
seams, padding, corner/custom normals, transformed meshes and actual rendering.
