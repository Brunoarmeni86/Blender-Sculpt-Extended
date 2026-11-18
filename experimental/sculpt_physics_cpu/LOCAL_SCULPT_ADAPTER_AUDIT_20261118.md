# Normal Sculpt + Multires Adapter and Local XPBD CPU — experimental audit

**18 November 2026.** Standalone C++17 reference, not linked to Blender or a released build.

## What was implemented

- `World::step_local(dt, substeps, iterations, bond_hops)`: optional **brush-local** XPBD step. A BFS finds active particles from selected Grab targets through the material-bond graph, then solves only affected bonds. Bonds crossing the active island boundary treat outside particles as fixed (approximation).
- `World::last_step_statistics()`: reports processed particles, constraint passes, and global fallback.
- Global gravity, floor collision, or a step with no brush targets falls back to `World::step` to avoid silently dropping global forces. Other dynamics outside the active island are **not** properly advanced in local mode; use only as a quasistatic sculpt prototype.
- `SculptSurfaceBridge::for_regular_mesh`: 1:1 proxy particle to a **normal Sculpt Mode mesh vertex**. No Multires modifier is required.
- `SculptSurfaceBridge::for_multires_samples`: barycentric anchors to a coarser proxy and an offset preserving each fine sample's detail. Offset is currently *object-space* and not tangent-frame transport, so it can drift or rotate incorrectly under large poses.
- `compose_positions` separates `Standard` (exact ordinary brush positions), `PhysicalAssist` (blend between artistic and physical result) and `FullPhysics` (physical result). It performs no Blender writes; the editor must manage masks, Undo, layer updates and PBVH.
- Added tests for no-Multires mesh, direct/full/blended brush composition, Multires detail preservation, locality, and fallback.

## Standalone CPU measurements

Release C++17, GCC, single-thread Linux virtual CPU; each line is median of 5 repetitions following warmup. 4 substeps × 5 iterations, 0.2 brush radius, cubic lattice, 0.075 spacing, 1e-5 bond compliance. **Solver step only:** excludes `begin_stroke` snapshot, brush region scan, Blender PBVH/Multires, rendering and transfer. Output is reproducible with `bench_local_physics`.

| Particles / bonds | Global step | Local 2 hops | Local 4 hops | Local 8 hops |
| --: | --: | --: | --: | --: |
| 8,000 / 22,800 | 6.868 ms | 0.357 ms | 0.873 ms | 2.938 ms |
| 27,000 / 78,300 | 24.170 ms | 0.366 ms | 0.858 ms | 3.471 ms |
| 125,000 / 367,500 | 138.746 ms | 0.421 ms | 1.038 ms | 3.623 ms |

### Fidelity tradeoff (single dab; 8k particles)

Compared to a global 4×5-iteration step with the same inputs; differences in particle position in **scene units**, evaluated within a radius of 0.35 of the brush center:

| Bond halo | Maximum position difference near dab | RMS difference near dab |
| ---: | ---: | ---: |
| 2 hops | 0.03019 | 0.00519 |
| 4 hops | 0.01823 | 0.00270 |
| 8 hops | 0.000568 | 0.0000522 |
| 12 hops | 0.0000000444 | 0.0000000047 |

Four hops is *not* automatically precise enough: in this scene it has ~0.018 maximum nearby error for a 0.1-unit target. Eight hops is substantially closer for these parameters, with corresponding increased solver work. Different materials and stroke histories can behave differently. Implement an adaptive halo with residual/boundary error tests before calling the local solver production-ready.

## Known limits

- Snapshot/Undo still duplicates **all** particles and bonds at stroke start (not included in step timings); needs sparse journal and Blender Undo integration.
- Grab search still scans all particles per dab; needs spatial grid when scaling further.
- No real Blender Sculpt brush registration, standard geometry callback, PBVH dirty update, Dyntopo, Multires CCG sampling or per-layer delta writeback.
- No particle-particle collision, volume/incompressibility constraint, anisotropic materials, proper contact friction, or cache/timeline.
- Global/full and local/approximate constraint order can diverge; neither is a calibrated clay material model.
- Mask Extract HR currently clips explicitly supplied triangles, not native Blender SubdivCCG grids.
- Per-corner UV transfer remains a separate reference operation. New cut/fracture interior mapping and full UDIM-preserving correspondence need more research.

## Test gates

- Release and Debug with AddressSanitizer/UBSan: standalone CTest **2/2 passing** (existing core tests plus new normal sculpt / Multires / local tests).
- CPU-only, no NVIDIA dependency.
- **Next:** benchmark and improve adaptive halo, sparse snapshot and spatial brush selection; then connect Standard/Assist/Full to the native Blender stroke transaction for base meshes, followed by Multires layer writeback. Record real stroke p50/p95, save/reopen, Undo/Redo and UV seam integrity.
