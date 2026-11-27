# D13 dedicated sculpt engines — research checkpoint

Base: D12 6d9e5b57e39a. Preserve D12 Clay/Flesh and A3/D12 Smooth math and dispatch. No changes to editor behavior in this checkpoint.

## Status
- A: standalone sample/stroke contract with distinct write permission, influence, area, swept support; not yet native PBVH adapter.
- B: CPU reference one-sided scraper and swept V-profile chisel. No editor integration; actual material removal requires topology support.
- C: bounded pairwise conservative flux primitive and separate Mud (yield), Push (upwind donor), Gather (radial convergence) laws. Chart construction, velocity/pressure solves, Smear, Press, Compress, Compact, Wax, Fabric, Foam and advanced Flesh remain to implement.
- D: not implemented. Thermal and granular volumetric models must remain separate from normal sculpt.
- E: not implemented. GPU kernels need deterministic edge-flux evaluation, budget reductions, and gather writeback; compare conservation and parity against CPU before enabling. Per-vertex scraper/chisel map naturally to independent threads. No CUDA claim.

## Acceptance gates
Use live positions, continuous swept contact, read-only halo, explicit masks, no cross-sheet transfers, bounded local collision guard, native Undo/PBVH writeback. Reject unsupported modes explicitly. Avoid global strength reduction and never silently substitute native Smooth. Validate independent brush signatures on planar, convex, concave, thin and irregular-density surfaces. Preserve area-weighted volume in transport before writeback; revalidate after guard. Fixed mesh volume change is only an approximation. Test CPU and CUDA separately; benchmark entire dab not just kernel. D12 remains checkpoint.

The added test target is standalone C++17; compilation and runtime are not verified here.
