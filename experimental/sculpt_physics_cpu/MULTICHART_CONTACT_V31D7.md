# V31-D7 — two-chart fold contact experiment
Base: abb3149b4896 (V31-D6). Keep BSE-Physical-V25 assets.

## Scope and mechanism
Opt in with BLENDER_SCULPT_MULTICHART_V31D7=1. Otherwise the existing
solver remains unchanged. Only supported live-surface Mesh Clay/Clay
Strips continuous V31D2 contacts in the original V30E footprint.
No Dyntopo, Multires, active shape keys/deform modifiers, plastic
compaction attribute path, or mirror/radial/reference/tile symmetry.
Test with all symmetry disabled. Deposit retains the original path.

Compute real incident dual vectors and area, select view-facing
positive-factor vertices inside the existing geometric region and
follow real connected edges from the nearest supported tip vertex
with bounded geodesic distance. A second chart is attempted only
if at least two supported steep vertices are detected (alignment
to original tool axis below .30). The second axis comes from a
small normal cone around the selected steep seed. Preserve the
original primary axis and radius; no new vertices outside the host
falloff. Split host influence between the two charts with normalized
smooth angular weights. Reuse existing material solvers independently,
then sum the resulting 3D motion and validate the full incident triangles.
Use current local center and plane for chart 2. Existing coherent
gradient/contact guard and editor guards remain. D6 tangent is not added
to a successful D7 multi-chart result; D5 dose settings are preserved.

This is a bounded experimental two-chart fallback, not a full surface
atlas. It can change pressure/transport behavior on curved patches.
Angle-based front-facing screening is not full visibility or CCD;
connected occluded surfaces may still require further work. No guarantee
of exact finite-step volume conservation. If chart 2 produces no
accepted steep motion, retain the original solver. Additional work
is limited to gathered local geometry, but two solver passes can cost
more on a fold. No global mesh scan is added.

Optional BLENDER_SCULPT_DIAG_V31D7=1: report one local sample every
32 attempted dabs, with eligible/connected/steep/charts/recovered and
scale. Recovered counts vertices with nonzero chart-2 motion and low
original alignment, not proof of a previous zero target on every vertex.
Keep PROFILE_V26=0, FOOTPRINT_AUDIT_V30E=0, DIAG_V31D5=0 for artist tests.

## Tests
sculpt_multichart_contact_v31d7_tests: connected 90-degree fold,
single-chart zero motion on its wall vs two-chart cut, masks, detached
sheet, triangle guard, flat no-op, masked seam and zero dose.
Existing D4/D5/D6 and material tests remain registered.
New tests have been written, not compiled or run in this session.

## Manual A/B
Use identical mesh, stroke, large brush radius, pressure and normal
radius with D7=0 and D7=1. Start with Scrape and Melt/Smear on the user's
narrow fold; verify other brush actions and normal surfaces. Check
recovered motion, accepted scale, mask/alpha, other side, Undo, time.
Do not change strength or Normal Radius in the comparison. If the
original host mask is already zero on the missed region, D7 will not
override it. A separate raycast/host-factor audit is then required.
