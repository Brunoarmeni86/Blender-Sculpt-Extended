# V31-D8A — narrow supported contact
Base: abb3149b4896 (V31-D6); D7 not included. No brush asset changes.

The user reports that a large brush stops deforming a narrow portion
of material, while a smaller brush works. This does NOT prove that
actual wall thickness is the cause: the V31 solver has no measured
solid thickness. Distinguish projected writable support, receiver
capacity, and post-planner stability before redesigning material physics.

## D8A: first isolated correction
BLENDER_SCULPT_NARROW_RIM_V31D8=1 enables a smaller internal receiving
profile in Press, Compress and Compact on supported live-surface Mesh.
The old radius was clamp(max_r/.88,.25,1); new lower bound is .025.
max_r is the maximum projected normalized distance of the already
selected vertices with weight > .02. This does not widen or shrink the
native brush footprint. The profile is unchanged for max_r >= .22.
The original signed return budget, cap on berm height, masks, geometry
guards and material action remain. This is not physical wall-thickness
estimation, nor a shell solver. Long narrow strips with large max_r may
not benefit. No change for Scrape, Deposit, Push, Melt or Smear.

BLENDER_SCULPT_DIAG_V31D8=1 logs one local sample every 32 planned dabs.
Works with D8A disabled too. Reports host eligible, solver active,
goals, chart/component rejects, receiver counts, extent_R, receiver
(sum of projected weighted areas across substeps), removed (sum of
absolute requested volume), minimum rim_accept and minimum solver scale.
rim_samples counts rim solves including patch splits/substeps, not
native dabs. rim_adapted counts reduced internal rim profiles, rim_empty
counts zero receiver. The sample is local; no whole-mesh audit.
To examine executor/writeback separately enable the existing
BLENDER_SCULPT_DIAG_V31D5 on a short diagnostic run only.

## D8B onward — proposed, not implemented
1. Measure local thickness with bounded inward ray intersections against
   evaluated geometry, correct oriented opposite-facing hits, ignore
   self/adjacent faces. No hit or ambiguous/open geometry = unknown,
   not a zero thickness. Separately estimate the two principal widths
   of supported connected contact (weighted surface moments). Distinguish
   wall thickness from narrow writable contact due to masks or topology.
2. If stalled response is verified, adapt proxy spacing/transport reach
   to the measured support width, preserving host footprint and dose.
3. Only for confirmed thin material, prototype action-specific bounded
   shell bending/tangential motion. Pair opposite surfaces where verified
   to avoid crushing, preserve masks, avoid self-intersection, validate
   combined 3D motion, keep broad geometry bitwise original. No generic
   fallback if alpha/mask/dose is zero. A shell response is not exact
   volume conservation. GPU acceleration follows artist validation.

## Verification
CTest sculpt_narrow_rim_v31d8_tests builds a tiny planar supported patch
inside a large brush. The intended regression tests the old zero-receiver
case, the new nonzero response, unchanged broad support, pinned and
disconnected geometry, signed first-order volume and substep stats.
It is a support-width test, not a closed thin-shell thickness test.
Tests are registered but have not been compiled/run in this session.
For manual A/B preserve the exact working D6 settings and assets, compare
D8A=0 and D8A=1 at identical brush size on Press/Compress/Compact.
Use Deposit and Scrape as controls. If those also stall, prioritize
host factors, chart rejection and writeback limiters instead.
