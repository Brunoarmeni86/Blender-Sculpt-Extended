# Local Multires normal-update face selection

nodes_to_face_selection_grids previously allocated and scanned a boolean for
every base face, even when only a small PBVH region changed. For selections
with fewer grid references than one eighth of the base-face count, it now
collects mapped face indices, sorts and deduplicates them, then constructs the
same IndexMask. Broad updates retain the parallel dense path. Empty updates
return immediately. A serial node-count pass bounds sparse scratch usage.

The normal arithmetic, stitching, GPU buffers, layer payloads, brush behavior
and approved profile are unchanged. Cost on dense base topology can now depend
on the touched region instead of the whole base mesh. High subdivision on a
tiny base mesh generally retains the old path and may see little benefit.

The production sort/dedup helper passes 288 comparisons against an independent
full boolean selection, covering empty, repeated and out-of-order face IDs.
MSVC /O2 isolated reference benchmark, 200 iterations, 1M base faces and 128
selected IDs: dense reference 77.633ms, sparse 0.146ms. This compares selection
algorithms only, not complete Blender/TBB execution or viewport frame times.
No overall Sculpt speedup percentage is claimed. Release compilation and the
existing focused startup and masked Subdivide/save/reopen checks are required
before pushing this change. Manual dense-model performance remains user work.
