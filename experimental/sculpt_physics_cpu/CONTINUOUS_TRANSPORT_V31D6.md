# V31-D5/D6 — continuous contact and tangential surface transport
Base: V31-D4 ea26ebccdee1. Keep the original BSE-Physical-V25 assets.
No V32/V33 code or asset recipes. Every switch defaults to off.

## Independent experiments
- BLENDER_SCULPT_DOSE_V31D5=1 (A): moving SPACE dabs receive a bounded
  proportional gain (BLENDER_SCULPT_V31D5_DOSE_GAIN=1.30, range 1..2).
  This is the equivalent distributed energy of repeated short strokes,
  not a new physical impulse or an extra dab. First dab, stationary dose
  and gap handling stay original; clamped to 1.0 as before. Subdivision
  invariance of the scalar input only holds below that clamp.
- BLENDER_SCULPT_REFRESH_V31D5=1 (B): mark local tool-plane state for
  refresh when crossing spatial interval BLENDER_SCULPT_V31D5_REFRESH_R
  (default .50 R, range .20..2). In live-surface mode geometry/plane already
  refresh every dab; the effective addition for Push/Smear is the local
  re-anchoring of the filtered transport direction after the normal
  entrainment step, preserving transport load and all native stroke state.
  No artificial click, Undo split, extra timer or geometry reset.
- BLENDER_SCULPT_NATIVE_DOSE_V31D5=1 (C): for an existing supported
  Adaptive Stroke with Uniform Accumulation enabled, use .2*overlap_factor
  as the reference sampled dose for moving, non-gap SPACE dabs.
  The ratio is sampled screen/radius spacing, not exact surface arc length.
  Falls back to original physical dose when unsupported or invalid.
  Does not enable Adaptive Stroke automatically or multiply overlap twice.
- BLENDER_SCULPT_DIAG_V31D5=1 (D): report dose and refresh at each input,
  and sums of displacement norms (requested, executor, after guards and
  actual eval position difference after writes). These are motion budgets,
  not volume or a net vector; no-write targets/executor reported explicitly.
  Existing clipping, collision and error guards remain.
- BLENDER_SCULPT_TANGENT_V31D6=1 (D6): only moving Push/Smear on
  regular supported Mesh with continuous contact and V30B transport.
  Export selected positive weights from the V31 solver, intersect across
  internal material substeps. Add tangent component projected onto the
  actual local oriented dual area; capped by .04R and .10 local minimum
  incident edge length (density-dependent safety, not full resolution
  invariance). Gain BLENDER_SCULPT_V31D6_TANGENT_GAIN=.18, range 0..0.50.
  Backtrack optional component if the original coherent/contact guard
  would be materially worse (>2% relative reduction); retain existing
  editor writeback validation. No new targets on masked/rejected sheets.
  Additional first-order swept volume is approximately zero, not exact
  finite-step volume/mass, UV preservation or volumetric advection.
  Topology and UVs are unchanged; vertices and their attached attributes
  move. Original strict clip/axis-lock checks can refuse a stroke.
  Actual Blender behavior, collision/Undo safety and density parity still
  need tests. Multires/Dyntopo/true GPU/MPM/tearing are not implemented.

## A/B acceptance
Keep REACH_V31D4=0 initially for the D3 reference and preserve other D3
environment settings. Enable diagnostics in every run. Compare D3,
A only, B only, C only (Adaptive Stroke + Uniform Accumulation), then
A/B/C combination and finally D6 only on Push/Smear with accepted settings.
Keep Sweep V29 unchanged during these comparisons; a separate sweep-off
control can be tested afterwards. Use identical radius, strength, pressure,
spacing, path and surface. Compare one long drag against repeated short
drags with equal travel, plus flat/curved mesh, mask, alpha, reverse,
axis-lock, undo, dense mesh and high/low spacing.
No need to regenerate brushes.

## Standalone tests
CTest sculpt_continuous_transport_v31d6_tests,
sculpt_contact_reach_v31d4_tests, sculpt_material_v25_tests.
New regressions exercise dose input subdivisions, first dab and gap,
refresh interval/load, disabled and zero-dose behavior, tangency,
fixed weights, sign and actual V31 contact weight export.
These tests are registered but have not been compiled/run in this session.
