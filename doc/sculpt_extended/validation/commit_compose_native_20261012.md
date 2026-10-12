# Commit-Compose native trace, 2026-10-12

User supplied commit-compose-20261012-161248.log and matching errors log.
The opt-in code executed at reshape/top levels 4 and 5 with live=0.
Both LAYER_COMMIT_REFERENCE entries report mode=SKIP_EQUAL_LEVEL but
skipped_samples=0: these commits contained no populated effective-layer
payload to skip. This validates execution of the gate, not the saved work
or correctness with populated existing layer contributions.

LAYER_COMMIT_OP ok=1 totals were 287.604 ms and 1061.305 ms. No paired
legacy measurements; different levels and sample counts, no speedup claim.
LAYER_UNDO_V2 STANDARD_SWAP appears twice at level 4. Geometry restoration
has not been confirmed separately for this candidate. Blender quit normally.
The errors log contains the previously observed unrelated HairBricks startup
TypeError, with no Commit-specific exception.

Keep Commit-Compose opt-in and provisional. A useful next coverage check is
another stroke/Commit on the same already recorded visible layer using the
global diagnostic launcher; skipped_samples should then exceed zero. The
new Commit-Parallel launcher instead exercises local Commit and does not
validate this global optimization. Do not confuse the two experiments.
