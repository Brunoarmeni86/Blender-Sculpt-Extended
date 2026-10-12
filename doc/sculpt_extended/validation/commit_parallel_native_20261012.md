# Native local Commit scheduling traces, 2026-10-12

Candidate production commit: 529b572b1cdbed3b2c44c999e34bcbe12a1a7ae6.
User supplied commit-parallel-20261012-162546.log and 163448.log, plus both
matching errors logs. No paired -Off session provided. Native candidate
execution confirmed by LAYER_COMMIT_WORK_PARALLEL; general correctness pending.

## Complete local Commit records

| Process | Level/area | Exact samples | Parallel | Solve ms | Local total ms |
| --- | --- | ---: | --- | ---: | ---: |
| 162546 | 4/81 | 64,810 | yes | 1.560 | 2.548 |
| 162546 | 4/81 | 239,580 | yes | 4.594 | 5.985 |
| 163448 | 3/25 | 45,103 | no | 8.372 | 9.344 |
| 163448 | 4/81 | 188,155 | yes | 3.749 | 4.611 |
| 163448 | 5/289 | 490,842 | yes | 9.927 | 12.124 |

All five completed records have LAYER_COMMIT_OP ok=1. The area=25 case has
48,475 candidate samples and correctly remains below the 65,536 threshold.
Area>=64 already used parallel solve in the legacy policy; these traces exercise
the new batching but do not validate the newly enabled small-grid parallel case.
No ratio between different level/geometry/stroke transactions establishes
speedup. Frame cache remains OFF and tangent solve LEGACY_INVERSE.

A later 876,267-sample parallel solve took 17.516 ms; the second log contains
no corresponding completed local publication/operator record or Blender quit.
Do not treat that last transaction as completed or infer a crash from absence.
First log ends with Blender quit. User has not separately confirmed geometry
and Undo/Redo for this scheduling candidate.

## Other measured costs and messages

Second log: complete CCG evaluation at area=289 took 1800.465 ms and complete
CCG build 1823.164 ms. Other modifier evaluations exceeded these times, but
operation association is not fully traced. A local Commit optimization does not
remove full reconstruction costs. Populated-layer Subdivide reports skipped=0,
consistent with the empty-layer-only candidate scope.

Both errors logs contain the known HairBricks startup TypeError. The second
main log contains geom.mesh.convert warning: Shape key data lost when replacing
mesh in Main. This message concerns a mesh replacement; no surrounding trace
identifies the triggering operator. Do not attribute it to the scheduling patch
or ignore its significance. The source change only adjusts solve dispatch.

Keep candidate opt-in and reversible. Limited native execution is positive;
paired speedup, expanded small-grid parallel coverage, geometry and Undo/Redo
remain unconfirmed. Main and baking remain unchanged.
