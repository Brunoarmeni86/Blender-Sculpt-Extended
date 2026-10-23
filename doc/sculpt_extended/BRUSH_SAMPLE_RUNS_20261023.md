# Multires brush dirty-sample journal

The shared grid apply_translations() writer previously staged one int64 sample
ID for every nonzero displacement, then set each global journal bit separately
while holding the shared journal mutex. It now stages consecutive sample runs
and uses MutableBitSpan::set_all() for each range under the same mutex.

Positions, nonzero detection, floating-point arithmetic/order, exact sample set,
grid flags and commit/Undo consumers remain unchanged. Runs preserve encounter
order and need not assume sorted PBVH grids. Existing bits outside each run are
preserved by the bit-span operation. This is not layer-detail isolation.

The compact staging uses two int64 values per run instead of one per changed
sample. Dense contiguous changes therefore reduce staging and mutex operations;
isolated alternating changes may use more staging memory and offer no gain.
No change to geometry resolution, brush strength, normals or native defaults.
The global journal bit-vector allocation is unchanged.

Applies to brushes calling the shared grid translation writer, including Draw,
Nudge and Clay. Dedicated writers (including portions of Smooth) are outside
this change. Mesh PBVH and Dyntopo paths are unchanged.

Validation: the actual run-builder header is compiled with MSVC /O2 and checked
against 1,230,080 original sample IDs with dense, alternating, random and spatial
support patterns, permuted grids, repeated IDs and 64-bit offsets. All expand
back to the exact input sequence. Release compilation is required separately.
No overall brush speedup percentage or dense-model benchmark is claimed.
Manual brush checks are left to the user as requested.

Candidate is based on cc027d9300cbf730f0f0d7feb0179a565e54d2dd (5.2.1).
The 5.2.3 update is audited separately and has not been merged.
