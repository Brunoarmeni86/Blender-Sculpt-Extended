Candidate based on Edit Mode checkpoint 3db7a10, separate from main and Bake.
Extract to C:\BlenderDev\Sculpt-Layer-Cache.

Close Blender, then Developer PowerShell:
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& 'C:\BlenderDev\Sculpt-Layer-Cache\APPLY-LAYER-CACHE.ps1'

Checks patch applicability first, saves backups, reuses configured build and INSTALL.
No new worktree/reconfiguration. If patch rejects, do not overwrite current files:
other-chat source changes need a selective rebase.

Launched test: open a COPY of a scene with a visible layer containing sculpt data.
Enter Sculpt, enable REC and make one stroke to exercise the CCG/Commit path.
F3 -> Sculpt Layer Cache Benchmark. Ten strength updates, index off/on.
Inspect strength, hide/show and Undo manually; this is not an automatic geometry test.
Close Blender so logs flush. Send native-layer-cache-bench.json + layer-cache-*.log.
First calls have cache warmup; timings exclude viewport drawing.
Do not save over your original test scene.

Runtime options (restart Blender for budget changes):
BLENDER_SCULPT_LAYER_CACHE_MB=512 : combined optional cache payload/lookup budget,
half for Commit frame cache and half for strength delta cache. Not total Blender RAM.
0 disables payload caches; absent keeps prior policy. Existing Commit frame cache
still requires its prior enable flags; this kit does not enable those experiments.
BLENDER_SCULPT_LAYER_SUPPORT_CACHE=1 : sparse support index, one hot layer.

Launch another test without recompilation:
& 'C:\BlenderDev\Sculpt-Layer-Cache\START-LAYER-CACHE-TEST.ps1' -CacheMB 1024

Remove only this candidate and rebuild:
& 'C:\BlenderDev\Sculpt-Layer-Cache\APPLY-LAYER-CACHE.ps1' -Revert

Native Blender compilation and validation are pending. main remains frozen.
