# V33 — Native sculpt footprint, V31 material behavior

Base: V31-D3 6b8b6a05cc30. Keep the original BSE-Physical-V25 asset library
and previous V31-D3 launch profile. Do not substitute the V32 recipe library.

## Scope
Enable BLENDER_SCULPT_FOOTPRINT_V33=1 on top of the known V31-D3 environment.
Disabled/unset retains the previous code path. V33 is restricted to explicit
material Clay / Clay Strips assets on regular supported Mesh topology with
live-surface accumulation, SPACE or AIRBRUSH. This is not a new physical solver.

* Clay: standard common mesh factors; Clay Strips: native cube tip, Z factors,
  plane trim, mask, alpha, hardness and automask.
* Existing PBVH broad-phase expansion can still gather support, but cannot
  authorize a nonpositive factor. Swept V29 footprint is disabled for this
  opt-in comparison, so normal native stroke sampling owns the footprint.
* The surface planner no longer re-applies the axial slab, chart radius or
  depth feather in V33. Native positive factors remain authoritative for
  eligibility, but real positive projected support, finite geometry, local
  topology and bounded reach still gate the physical chart. This is not a
  claim that every native target must move (e.g. scrape contact).
* Incident triangles add fixed support vertices. Zero-factor vertices cannot
  receive goals or bridge transport. The existing V31 topology limitations
  remain, including disconnected writable islands in some solver paths.
* Replace the extra global differential/slope attenuation with the existing
  local triangle limiter and signed budget rebalance, followed by final
  incident-triangle validation. A common final guard can still reduce motion.
  First-order projected budget only, not exact material mass.
* Reuse the V31 distinct actions, material controls, pressure curve, stroke
  dose and live-surface update. No new asset recipes or RNA fields.
* Log [V33 sculpt] eligible, active, goals, chart and component rejects,
  local limits and final common scale. Compare with V30C write actual moved.
  Counts are per dab; substep rejection counts may accumulate.

## Validation before acceptance
Build standalone test_sculpt_footprint_v33 and test_material_v25 and execute
CTest sculpt_footprint_v33_tests and sculpt_material_v25_tests. New regression
covers a host-selected patch beyond the former depth slab, fixed halo,
detached sheet, influence ordering, substeps, zero pressure and topology
barriers. No runtime results are claimed by this commit.

Blender A/B: same original V25 assets, same mesh and stroke, V33 disabled
versus enabled. Test flat and curved surfaces, steep relief, thin sheets,
masked strips, alpha, small/large radius, tablet pressure, stationary dabs,
continuous strokes, symmetry, undo and high-density latency. Distinguish
intentional contact restrictions from missing writable vertices. Confirm
that each old material action is visually distinct and that geometry does
not fold or tear accidentally. Do not approve until artist tests pass.

## Next
GPU Warp port after CPU correctness: compact workset, topology cache,
persistent device buffers, deterministic/colored constraints, transfer-
inclusive p50/p95, CPU parity and fallback. Keep native editor ownership of
writeback and undo. True Liquify requires persistent volumetric material
points/grid or fluid particles with surface coupling; Tearing requires
fracture criteria plus a topology-changing transaction with mesh/UV/mask/
attribute/undo handling. Neither is implemented by V33; Multires cannot
generally represent topology-changing tears without a separate policy.
