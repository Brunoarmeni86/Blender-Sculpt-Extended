Extract to C:\BlenderDev\Commit-Subdivide-Memory.
Based on cache-only checkpoint fa4998d. Keep strength-zero fix removed.
main and bake branch unchanged. Both additions are opt-in and reversible.

Close Blender, then existing Developer PowerShell:
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& 'C:\BlenderDev\Commit-Subdivide-Memory\APPLY-COMMIT-SUBDIVIDE.ps1'

Incremental compile + INSTALL to existing shareable. No new build or CMake reset.
If patch rejects current other-chat source changes, no files are overwritten.

Small automatic Subdivide test (close Blender first):
& 'C:\BlenderDev\Commit-Subdivide-Memory\TEST-EMPTY-SUBDIVIDE.ps1'
Return native-empty-subdivide-comparison.json and both native-empty-subdivide-*.json.
Fresh tiny cube, three empty layers, one subdivision, save/reload. No Undo claim.

Commit global-path test, using a SMALL scene copy first:
& 'C:\BlenderDev\Commit-Subdivide-Memory\START-MEMORY-TEST.ps1' -Mode Commit -ForceGlobalCommit
Sculpt at maximum level with REC, one stroke, Commit, inspect shape and Undo.
Close Blender and return commit-subdivide-*.log. Normal local Commit usually
bypasses the global reshape, so it will not exercise this optimization.

Flags:
BLENDER_SCULPT_COMMIT_SKIP_ORIGINALS=1 skips original-grid deep copy only at equal
reshape/top levels. Lower-level propagation unchanged. Local Commit unchanged.
BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE=1 keeps entirely empty layers sparse.
Partial/populated layers, hidden bitmap data and nonzero L0 use legacy subdivision.

Start baseline behavior without compilation:
& 'C:\BlenderDev\Commit-Subdivide-Memory\START-MEMORY-TEST.ps1' -Mode Off
Remove this source candidate + rebuild:
& 'C:\BlenderDev\Commit-Subdivide-Memory\APPLY-COMMIT-SUBDIVIDE.ps1' -Revert

Native compilation, runtime memory, shape and Undo tests are pending.
These patches do not remove all Commit/Subdivide temporary buffers or guarantee
that extreme-resolution memory crashes are resolved. No strength-zero changes.
