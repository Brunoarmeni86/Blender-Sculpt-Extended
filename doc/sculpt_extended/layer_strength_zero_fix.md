# Strength-zero routing correction and native cache log review

Parent performance branch fa4998d. The user's logs show native index BUILD/HIT,
34.2 MiB hot weight cache and approximately 48-52 ms warm strength updates.
Existing frame cache/fast tangent solve are OFF in that run. Uploaded stderr
contains draw profiling, not a crash traceback. Another run includes an invalid
operator invocation and re-registration attempts. Latest benchmark log starts
with empty payloads, then records a populated layer, but ends mid-line before
complete off/on results. No native benchmark JSON was supplied. Do not claim
measured index speedup, budget-bypass validation or full Undo correctness.

The log also records strength -3 -> 0 as METADATA_ONLY, followed by 0 -> .2
as DIRECT_DELTA. This reveals a pre-existing RNA guard: it looked at NEW strength
rather than weight change, skipping removal of the old contribution. The cache
candidate did not introduce that guard.

Fix: metadata-only only for zero effective weight change; invalid weight selects
conservative rebuild. Compute effective old/new weights using composition's 1e-6
threshold, and use effective strength for visibility toggles too. Keep L0/shrinkwrap
and unsafe-runtime safeguards. Grid zero transitions now reach the same existing
direct delta or evaluation fallback as ordinary changes. Layer delta storage and
Undo representation are unchanged.

Extracted actual guard test with g++ warnings-as-errors passes nonzero->zero,
zero->nonzero, near-zero threshold and invalid-weight cases. This does NOT test
real CCG normals, geometry, Undo or native Blender compilation. Native fix remains
pending. Reopen a scene copy in a fresh process for validation rather than
continuing a CCG state that experienced the old skipped-zero update.

The repaired benchmark adds REGISTER for operator search and includes a zero
transition in its ten updates. The repair kit applies only the RNA correction on
top of the installed cache patch, backs up and builds existing INSTALL. Leave it
for the next session; no further user testing is required to retain these logs as
partial evidence. Main and bake branch remain untouched.
