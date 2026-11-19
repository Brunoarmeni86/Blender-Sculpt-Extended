# V6 — Hybrid Sparse Stroke Undo and Persistent Local Solver Workspace

**18 November 2026 | Standalone CPU C++17 experimental branch, Draft PR #21 | NOT a Blender-integrated brush**

## Main changes

The V5 CPU reference copied all particles and bonds during `World::begin_stroke()`, even for a missed dab or a six-particle interaction inside a 125,000-particle scene. V6 introduces a **first-touch stroke journal**:

- `remember_particle(index)` and `remember_bond(index)` record the original value only once per stroke; later dabs do not overwrite it. Local stepping journals active dynamic particles and all touching XPBD bonds *before* integration, preserving original position, velocity, plastic rest length, compliance, fracture state, and multiplier.
- `begin_stroke()` clears journal state rather than copying full arrays; cancel restores journaled originals; commit discards the recorded history. The existing sparse moving-particle index is snapshotted and restored separately.
- **Hybrid global fallback:** `World::step()` upgrades to a contiguous full-state snapshot at the first global step. It overlays the original sparse pre-stroke values onto the current world copy before discarding maps. This avoids millions of individual hash entries during genuinely global dynamics and ensures local→global→cancel returns to the original state.
- `stroke_journal_statistics()` exposes count and approximate raw payload size (excludes unordered_map buckets, nodes, vector capacities and allocator overhead).
- **Persistent scratch:** `step_local_impl` reuses active vertex IDs, selected bond IDs, start/previous position buffers and an inclusion generation-stamp array. A full O(N) mark reset is needed only for initial allocation, resize or extremely rare 32-bit counter wraparound. This synchronous `World` is **not reentrant or thread-safe**; parallel access needs an additional design.

The adaptive halo solver continues to record **per-attempt temporary checkpoints** for candidate rollback. This checkpointing is separate from stroke Undo, which records first-touch **pre-stroke** state. Rejected candidates may be present in the sparse journal; undo correctness is maintained, but future work can avoid their retained memory.

## Tests

New seventh suite `sculpt_sparse_undo_tests` covers:
1. Start/commit and a missed dab on a 20k-particle world: **zero** particle/bond Undo entries before any change.
2. Sparse local stepping on a large world: a couple of saved particle states and one bond, with no duplicate records on repeated dabs; cancel restores exact positions and velocities.
3. A plastic deformation followed by global gravity/fallback and hybrid journal upgrade: cancel restores pre-stroke particle and bond material state, including rest lengths and multipliers.
4. Local bond fracture plus a subsequent distant dab: no overwrite of the original fracture state and correct cancel.
5. Commit and a subsequent stroke maintain independent original histories.

Local standalone verification: **7/7** in GCC Release, Clang Release and GCC Debug ASan/UBSan. Do not conflate these with a built Blender executable. Windows CI must be confirmed for the exact commit.

## Timing (one virtualized Linux CPU, optimized C++17)

Same benchmark code `bench_stroke_undo.cc` linked against the V5 baseline and V6. 8 measured repeats following one warmup for `begin_stroke()` and local stroke; 5 measured repeats for global. All values are approximate **medians** (ms); virtualized CPU and allocations introduce noise.

| Particles | Bonds | V5 begin | V6 begin | V5 local stroke | V6 local stroke |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 9,999 | 0.1121 | ~0.0001 | 0.1159 | 0.0216 |
| 50,000 | 49,999 | 0.5714 | ~0.0001 | 0.6708 | 0.1279 |
| 125,000 | 124,999 | 2.2380 | ~0.0001 | 1.9440 | 0.4440 |

This is a **single fixed-radius Grab at the start of a long bonded chain**, `dt=1/30`, one substep, two XPBD iterations and a two-bond-hop island. The displayed `begin` values approach timer resolution and must not be interpreted as physical zero-cost allocations. The local stroke timings include `begin_stroke()`, `apply_grab()`, and `step_local()`, but exclude `cancel_stroke()`, native Blender PBVH/CCG transfer, viewport refresh, native Undo, and GPU synchronization. `apply_grab` still scans all particles and dominates at large N. The geometry is a narrow chain, **not** a dense clay volume.

For global strokes in the same benchmark, V5 vs V6 medians were ~0.892 vs ~0.856ms at 10k and ~4.77 vs ~6.28ms at 50k. This variability and the V6 50k regression **must** be reprofiled: journal upgrade and contiguous snapshot memory traffic can offset local savings. No blanket speedup claim for global dynamics.

## Outstanding correctness/performance work

- **No native Blender Undo/Redo:** `cancel_stroke()` only restores the standalone simulation world; it does not write back to Blender, invalidate PBVH, restore evaluated Multires CCG grids, or integrate into Blender Undo stack.
- **Exception atomicity:** raw `step()`, `step_local()`, and `step_local_multi()` can throw mid-step and leave mutated data; adaptive/global fallback paths have some explicit checkpoints. The future native brush adapter must not write to the artist's mesh until the simulation step has completed successfully, and will need a robust per-step rollback.
- **Sparse journal scaling:** `std::unordered_map` entries have significant allocator/node overhead; implement chunk/block first-touch snapshots or generation-stamped flat indices if profiling shows hash overhead at medium region sizes. A dense upgrade threshold may be justified before the global fallback.
- **Full-world snapshot on true global dynamics:** unavoidable for exact pre-stroke Undo under the current state model, but ideally rely on Blender's Undo implementation when integrated. Current maps are not production serialized.
- **Brush search stays O(N)**; introduce a dynamic spatial hash or PBVH-derived query. `adaptive_solver.cc` still allocates its BFS distance and traversal arrays per attempt, and temporary candidate snapshots; reuse scratch after establishing ownership/reentrancy policy.
- **Surface transport:** normal Sculpt bridge is 1:1, which is too expensive for very dense meshes. Both normal Sculpt and Multires should support a decoupled lower-density physics proxy, with stable topology/attribute provenance.
- **Material fidelity:** no calibrated clay volume/pressure model, robust contacts/friction or full-body dynamics. The local multi-island solution is approximate near artificial graph boundaries. Separate boundary reaction checks remain necessary.

## Acceptance gate

Keep PR #21 Draft. To consider native Sculpt integration, require exact-commit Linux/Windows CI, multistroke tests of local/global/material state, per-step exception rollback, profiling including undo/brush selection/dirty surface writeback, then the opt-in native Grab adapter with Standard/0% bypass. Preserve Sculpt Layers, masks, UVs and high-frequency detail in the later Multires bridge. No changes to the approved release build.
