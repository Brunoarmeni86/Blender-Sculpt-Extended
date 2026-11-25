# V31-D4 — isolated contact reach experiment
Base: 6b8b6a05cc30 (V31-D3). No V32 or V33 solver changes.

Enable BLENDER_SCULPT_REACH_V31D4=1 with the previous V31-D3 profile
and original brush assets. Default/off retains the original D3 path.

Only continuous V31D2 contact gets an additional 0.15 brush radii of
graph path reach; the geometric/PBVH candidate bound, seed bound,
edge eligibility, positive projected support, masks, alpha, original
inner plateau, averaged-normal weighting, action solvers, transport,
writeback and stability guards remain unchanged. The outer feather
widens from .25R to .40R. No flux across zero-factor barriers or
disconnected sheets is newly allowed. Normal averaging on the expanded
contact set can affect nearby weights. This is not guaranteed to fix
all rejected vertices or improve artistic behavior.

A/B on the same executable: set BLENDER_SCULPT_REACH_V31D4=0 versus 1
before launch, with otherwise identical settings, assets, mesh and stroke.
Compare V30E audit, V31D2 contact, V31D4 reach and V30C write. Prioritize
Melt and Smear; inspect Press/Compact/Scrape for unintended changes.
If worse, keep D3 and disable the experiment. Do not change stability
or strength simultaneously.

Standalone CTest: sculpt_contact_reach_v31d4_tests and
sculpt_material_v25_tests. The new test uses a synthetic winding graph
and tests outer recovery, original inner plateau with uniform normals,
bounded reach, mask barriers, detached sheet rejection and default/off
equivalence. Tests and Blender runtime have not been executed here.
