# Native Windows validation, 2026-10-12

Candidate: 23291f2b1d12b157df27701c79f51a94cb907cf2, cache-only parent fa4998d.

## Empty-layer Subdivide

User supplied native-empty-subdivide-comparison.json. Fixture preserved three layers, three levels and 386 evaluated vertices across OFF/ON, including save/reload. Max coordinate difference 1.7881393432617188e-07 (tolerance 1e-6). Timings: legacy 8.5595 ms, sparse 2.5972 ms; single small fixture, descriptive timings, no robust speedup claim. Native peak memory not measured. Populated layers and Undo not covered by this automatic fixture.

## Equal-level Commit

User supplied commit-subdivide-20261012-155125.log and reported shape and Undo correct in the small manual test. Trace contains three LAYER_COMMIT_ORIGINAL_GRIDS mode=SKIP_EQUAL_LEVEL reshape=3 top=3 entries, a successful LAYER_COMMIT_OP ok=1 total=408.592ms, and LAYER_UNDO_V2 mode=STANDARD_SWAP pending_preserved=1. Thus the intended skip path executed, rather than only an ordinary local Commit. No paired timing or peak RSS measurement; no general Undo correctness claim.

Error logs contain an unrelated HairBricksPreferences Python TypeError during addon startup. No Commit/Subdivide exception or crash observed in supplied logs; Blender exited normally.

Keep both candidates opt-in and reversible. Lower-level Commit propagation, large scenes, populated/partial/hidden layers, Conform and Edit/Sculpt topology Undo remain outside this validation. Main and bake branch unchanged.
