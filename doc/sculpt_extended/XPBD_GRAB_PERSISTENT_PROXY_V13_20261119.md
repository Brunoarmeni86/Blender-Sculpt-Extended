# XPBD Grab persistent proxy V13 checkpoint

## Change

The experimental XPBD Grab now creates its CPU `World` once per Blender stroke and stores it in
the transient `StrokeCache`. Particle topology, edge bonds, solver scratch, velocity, and target
storage are therefore reused by subsequent dabs. The proxy is automatically destroyed with the
stroke and is never serialized in a `.blend` file.

The existing conservative safety contract remains unchanged: runtime opt-in, ordinary PBVH Mesh,
native mask/falloff sampling, no real symmetry passes, and `bond_hops = 0` so the solver cannot
write vertices outside Blender's existing Undo node coverage. The former fixed 20k vertex ceiling
has been removed after the proxy became stroke-persistent. Allocation or solver exceptions remain
transactional and fall back to Blender's standard Grab path; practical limits now depend on memory
and first-dab proxy construction time.

## Validation

- Source diff check: passed.
- Full incremental Blender Release build: passed with Visual Studio 2022.
- Runtime deformation, Undo, masks, layers, axis locks, and comparison against V12: pending user
  viewport testing.

This is an experimental performance/continuity improvement, not a stable-release merge.
