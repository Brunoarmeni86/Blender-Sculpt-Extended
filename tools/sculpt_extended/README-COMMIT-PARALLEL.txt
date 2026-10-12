Extract to C:\BlenderDev\Commit-Parallel.
Candidate based on ade161e, compatible patch context with the earlier cache-only
sources: affects only the local Commit solve dispatch and a diagnostic trace.
Does not include or require the rejected strength-zero experiment.

Close Blender. Existing Developer PowerShell:
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& 'C:\BlenderDev\Commit-Parallel\APPLY-COMMIT-PARALLEL.ps1'

Incremental compile + INSTALL of the existing shareable; no CMake reset.
Native Windows compile, geometry, Undo/Redo and performance validation pending.

Why: logs show area=9, up to 5.68 million dirty-grid samples, but parallel=0.
The legacy gate requires area>=64 regardless of total work. With opt-in
BLENDER_SCULPT_COMMIT_WORK_PARALLEL=1, use parallel solve if at least eight grids
and 65,536 candidate samples, without live procedural displacement. Batch roughly
4,096 samples per range. Otherwise stay serial. No math/publication changes.
The flag must be exactly 1. Unset/0 preserves the legacy scheduling policy.
The launcher activates local Commit and tracing; other cache/math flags inherited.

Small verification on a scene copy with a visible REC layer at maximum level:
1. Make a reasonably broad stroke, Commit, inspect geometry, Undo/Redo.
2. Close Blender. Send commit-parallel-*.log and its errors log.
Look for LAYER_COMMIT_WORK_PARALLEL parallel=1 and LAYER_COMMIT_LOCAL parallel=1.
Tiny strokes may stay serial. More than half the mesh or hidden/mask changes
may use global Commit, which this candidate does not speed up.
Existing Commit-Compose launcher forces global Commit; use this new launcher.

For a paired comparison, reopen the same saved scene in a fresh process:
& 'C:\BlenderDev\Commit-Parallel\START-COMMIT-PARALLEL.ps1' -Off
Repeat a comparable stroke/Commit. Differences in sample counts must be considered.
No claimed speedup until the native comparison. Whole Undo still includes global
CCG reevaluation and is not eliminated by this scheduling change.

Disable the candidate without compilation with -Off above.
Remove only this source change and rebuild:
& 'C:\BlenderDev\Commit-Parallel\APPLY-COMMIT-PARALLEL.ps1' -Revert
