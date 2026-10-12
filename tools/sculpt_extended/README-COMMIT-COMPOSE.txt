Extract to C:\BlenderDev\Commit-Compose.
Candidate based on 9dcf2f9: cache-only + Commit/Subdivide memory patches.
Keep the rejected strength-zero experiment removed.

Close Blender. Existing Developer PowerShell:
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& 'C:\BlenderDev\Commit-Compose\APPLY-COMMIT-COMPOSE.ps1'

Incremental compile and INSTALL of the existing shareable. No CMake reset.
The launcher forces global Commit to exercise this candidate and enables
the two previously tested memory flags. Use a small scene copy first.
At maximum Sculpt Level: REC, one stroke, Commit, inspect shape, Undo/Redo.
Close Blender. Return commit-compose-*.log and the matching errors log.
Trace should show LAYER_COMMIT_REFERENCE mode=SKIP_EQUAL_LEVEL.
skipped_samples > 0 confirms a populated visible layer was skipped.
Without populated layers this does not exercise the avoided sample loop.

Flag BLENDER_SCULPT_COMMIT_SKIP_COMPOSE=1 only skips the redundant per-sample
reference sum at equal CCG/top levels, without live procedural displacement.
Descriptor checks, L0 base deltas, conversion, staged delta validation, masks,
layer publication and failure handling remain unchanged. Local Commit unchanged.
Lower sculpt levels and live displacement keep the legacy composed reference.

This is a CPU work reduction. It does NOT eliminate the scratch mesh or claim
lower peak RAM. Speedup depends on populated layers and global Commit frequency.
No native Windows compilation or correctness claim before user testing.

Disable only this option without compilation:
& 'C:\BlenderDev\Commit-Compose\START-COMMIT-COMPOSE.ps1' -Off
This diagnostic launcher still forces global Commit.
Ordinary launches use the legacy composition unless the environment flag is set.
To restore normal local Commit in this PowerShell before launching directly:
Remove-Item Env:BLENDER_SCULPT_LOCAL_COMMIT -ErrorAction SilentlyContinue
Remove-Item Env:BLENDER_SCULPT_COMMIT_SKIP_COMPOSE -ErrorAction SilentlyContinue

Remove only this source change and rebuild:
& 'C:\BlenderDev\Commit-Compose\APPLY-COMMIT-COMPOSE.ps1' -Revert
