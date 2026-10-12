# Layer performance audit from native logs, 2026-10-12

Read user attachments layer-cache-20261012-150128.log, matching errors log,
layer-cache-20261012-144944(1).log and layer-cache-20261012-143249-errors(2).log.
Two other named attachments failed transfer and were not used. These are
earlier cache traces, not validation of the newer Commit-Compose candidate.

## Strength

Large fixture: 393,216 coarse faces, 1,572,864 CCG grids, nine samples per grid,
14,155,776 grid samples. Grid samples include duplicated boundaries; do not
interpret this count as unique vertices or polygon count. Active layer payload
occupied every grid. Index HIT consequently retained the whole-mesh workload.

Five warm populated strength changes took 380.163--385.631 ms. Direct-delta
stage took 73.982--75.496 ms; hot application itself 17.248--17.894 ms.
Stitch averaged roughly 99 ms, normals 99 ms; surface stage 219--223 ms.
The first cache construction took 248.685 ms and the first strength update
730.717 ms. Hot object-space delta cache uses 162 MiB here.

Smaller populated fixture: warm total changes approximately 6.7--7.5 ms.
Index OFF/ON difference is marginal there. Empty-layer metadata-only timings
are excluded from populated strength comparisons. Enlarging the cache alone
cannot remove stitching, normals, support validation or PBVH costs.

## Local Commit and Undo

Large fixture: eight local Commits, area=9, parallel=0, frame cache OFF and
legacy inverse. Solve ranged 1.566--1034.499 ms; total 2.864--1081.325 ms.
Representative 631,324-grid transaction: 5,681,916 samples, 1034.499 ms solve,
1051.196 ms total. It is unnecessarily excluded from parallelism by area>=64.

Whole CCG reevaluation around Undo took roughly 665--730 ms at area=9.
LAYER_UNDO_FRAME_FAST remained FULL with skipped_eval=0. Parallel local Commit
cannot remove that separate reconstruction cost. Modifiers and full CCG builds
also incur other costs. No general Undo fast path is introduced in this work.

## Candidate: total-work local Commit scheduling

Opt-in BLENDER_SCULPT_COMMIT_WORK_PARALLEL=1 replaces the per-grid area gate
with >=8 grids and >=65,536 candidate samples, retaining the live procedural
displacement serial exclusion. Range grain max(1,4096/area) batches small grids.
Default/unset keeps the previous gate and grain. A candidate sample count is
an estimate of loop work, not the exact number of changed positions.

The same solve_grid writes its exclusive staging slice and solve_ok element.
Cached-frame slots are assigned/pinned before solve; per-grid cache writes are
exclusive. The established larger-grid parallel solve already shares the same
read-only Subdiv evaluator. Validation and publication retain their ordering.
No tangent arithmetic, cache policy, dirty journals, sparse allocation, mask
publication or Undo restoration change. No additional high-resolution arrays.

Realistic next priorities: measure this isolated scheduling candidate first;
then optimize repeated whole-surface strength bookkeeping and boundary work
with correctness checks. Avoid reusing Undo frames across topology/state
boundaries without exact validation. The logs justify an opportunity, not a
promised speedup. Native Windows compilation and paired validation pending.
