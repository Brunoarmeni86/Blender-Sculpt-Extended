# Sculpt Layer cache budget and sparse support index candidate

Parent: Edit Mode checkpoint 3db7a10. Dedicated performance branch, two separate
production commits. No changes to main or bake/sampling-quality.

## Budget

BLENDER_SCULPT_LAYER_CACHE_MB is opt-in, read once per process. Absent/invalid
retains previous policy. Each existing cache gets half the requested total.
Commit frame capacity accounts for per-sample frame size and grid/slot lookup,
and is capped by actual grid count. Zero capacity releases payload/lookup buffers
and safely routes all grids to the existing noncached solve. Weight delta cache
checks float3 payload plus grid lookups before allocating; when too large it uses
the existing uncached parallel evaluator, not a partially stored delta cache.

This bounds the main optional cache payload/lookup allocations, not total RSS,
allocator overhead, small Vector capacity slack, temporary work arrays, layer
storage, CCG, normals, Undo or global Commit scratch mesh. Fixed half-budget split
is intentionally simple; dense layers may become slower when the hot delta cache
cannot fit. Frame cache remains enabled only under its existing flags. No speed
or large-mesh crash guarantee.

## Index

BLENDER_SCULPT_LAYER_SUPPORT_CACHE=1 caches occupied grid indices for one hot layer.
RNA support detection and direct delta application reuse the same owned index
copy, avoiding repeated scans of zero-payload descriptors on hits. Cache identity
uses layer/payload-descriptor pointer, grid count and level; occupied descriptors
are revalidated. Existing payload mutation invalidation also invalidates support.
New sparse allocation and layer free explicitly invalidate caches. Remap/resample,
Commit, merge and Undo retain their existing invalidation paths.

Only support discovery changes. Coarse adjacency expansion, PBVH node selection,
position stitching, normals, bounds, layer storage and Undo representation remain
unchanged. It still scans coarse support bitmaps/PBVH as before and stores one hot
layer, not an unbounded per-layer table. Cache depends on existing mutation API
contracts; new direct payload writers must invalidate it. Main-thread property
transactions remain serialized with sculpt publication as before.

## Validation

Small extracted actual support helper check compiles with g++ warnings-as-errors:
budget boundaries/zero/legacy policy; support BUILD/HIT; allocation/free/layout
invalidation. Selective patches apply and reverse against GitHub checkpoint.
No native Blender compile here. Windows kit reuses existing source/build after
--check and backup, then launches an interactive ten-update benchmark with native
trace. The user must inspect geometry/Undo; timings alone do not establish correctness.

Current Windows sources may contain later Polypaint/Anchor changes from other
chats. The kit rejects incompatible hunks rather than replacing complete files.
Use a test scene copy, with a populated layer and Sculpt CCG active. Benchmark
measures RNA + dependency-graph updates, not viewport draw, includes initial cache
warmup and is descriptive rather than a required speed threshold.
