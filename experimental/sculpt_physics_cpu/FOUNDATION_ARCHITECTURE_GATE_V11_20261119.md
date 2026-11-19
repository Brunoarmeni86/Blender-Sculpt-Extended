# V11 foundation gate — design contracts before native Blender integration

**19 November 2026 | CPU C++17 reference, Draft PR #21 | No released Blender changes**

## 1. Bugs confirmed and fixed

### Inverse-mass scale independence

The original XPBD Grab and bond solvers skipped constraints when
`denom = inverse_mass_sum + alpha <= 1e-12`, even for **positive, finite
denominators**. A rigid Grab at inverse mass `1e-14` therefore moved its
single dynamic particle by **0** instead of the requested **0.25** units.
A denominator should be skipped only if it is *nonpositive*, not merely
smaller than a world-unit-dependent constant.

All four Grab/bond code paths in global and local XPBD now use:
```cpp
if (!std::isfinite(denom)) throw std::runtime_error(...);
if (denom <= 0.0) continue;
```

This fixes a **mass-scaling correctness error**, not just a performance case.
Finite positive but extreme denominators may still create invalid multipliers,
which the existing numeric rollback guards must reject. Test all three
global/local/multi-island modes and exact stroke cancel.

### Bond distance overflow

`length(Vec3)` previously computed `sqrt(x*x + y*y + z*z)`, which can overflow
even if the vector's Euclidean length is finite and representable. A length
of `1e155` failed in a global step although `add_bond()` accepted it.

The solver retains the fast squared-length path for ordinary magnitudes and
falls back to `std::hypot(x,y,z)` only when the square is zero (nonzero
vector underflow) or not finite (overflow). Regression checks a `1e155`
rest-length bond and stable finite positions.

## 2. Native-friendly brush selection API: do not duplicate Blender PBVH

```cpp
bool World::apply_grab_candidates(
    Vec3 center, double radius, Vec3 displacement,
    BrushPhysicsSettings settings,
    const std::vector<std::size_t>& candidate_ids);
```

- Accepts **preselected proxy particle IDs** from a caller, e.g. the future
  Blender PBVH query / active grid range.
- Canonicalizes index order and duplicates for deterministic Gauss-Seidel
  constraint order; invalid IDs throw without replacing the previous Grab.
- Uses the same finite-validation and falloff path as the existing O(N)
  `apply_grab()`; the latter remains the reference and fallback.
- **Contract:** the caller must include *all* proxy particles inside the
  queried brush radius to get the exact same targets as a full scan.
  Preselection geometry in Blender requires its own correctness tests.

An indexed API is preferable to implementing a second deformable spatial hash
inside the standalone World while Blender already maintains PBVH spatial
acceleration. Collision broadphase (for dynamic self-contact) is a separate
future problem.

### Preliminary measured selection cost

Local Linux virtual CPU, optimized C++17, 190 timed sample pairs after warmup,
one reused World, three candidates already supplied. Bench source:
`bench_candidate_query.cc`.

| Total particles | Original full scan | Indexed Grab with K=3 |
| ---: | ---: | ---: |
| 10,000 | ~0.0176 ms | ~0.00010 ms |
| 125,000 | ~0.2322 ms | ~0.00022 ms |
| 500,000 | ~1.0109 ms | ~0.00094 ms |

These are tiny microbenchmarks near timing resolution and **do not include
PBVH discovery cost**, full solver time, cache updating, mesh writeback,
Blender native Undo or high-res CCG. They only demonstrate the asymptotic
advantage of passing a small known candidate set to the solver.

## 3. Architectural contracts to freeze before native Brush integration

The goal is a *single* physics backend for normal Sculpt and Multires, with
Blender's existing XPBD constraint library reused where practical.

### A. Solver domain versus Blender/editor ownership

- **World / backend:** physics proxy particles, immutable material topology
  within a stroke, distance/volume/contact constraint state, local island
  stepping and numeric validation. Keep reference C++17 ABI independent from
  editor-specific types.
- **Brush adapter:** mode, radius, influence, selected PBVH/grid points,
  Face Sets/masks, current tool pressure, symmetry and real-time stroke input.
  Do not store editor view state in solver nodes.
- **Surface transfer:** a many-to-one mapping from **normal Sculpt as well as
  Multires evaluated samples** to stable physics proxy IDs, with per-sample
  residual/detail and orientation. Current regular Sculpt bridge is 1:1; this
  must evolve without changing the core World API.
- **Transaction writer:** stage solved proxy results, validate them and only
  then write changed Blender mesh/CCG tiles and tag PBVH. The C++ journal's
  `cancel_stroke()` is **not** Blender Undo/Redo. The editor must own native
  Undo, rejected-dab rollback and real evaluated-surface invalidation.

### B. Stable identity and topology generations

Model native IDs explicitly: stable proxy ID, source vertex/CCG sample ID,
face/corner attribute provenance, active Sculpt Layer ID, and topology/Multires
generation. **Invalidate** PBVH candidate cache, anchor lookup and per-stroke
journal if topology, Multires level or coordinate domain changes. Refuse
unsafe mid-stroke remapping instead of silently applying offsets to different
source vertices. Preserve UVs/color attributes **per face corner**, not just
per proxy particle. Keep transfer anchored through pose/symmetry.

### C. XPBD interoperability and material roadmap

The Blender fork already contains
`source/blender/geometry/xpbd/GEO_xpbd_constraint_distance.hh`.
Do **not** freeze a second permanently diverging distance-constraint engine.
Audit an adapter/shared math layer and numerical tolerances before native
integration. The present reference is an isolated correctness harness.

Define typed constraint passes for distance/stretch, volume,
bending/shear, plastic yield, fracture and contact/friction. Their execution
order and ownership must be explicit so future CPU coloring/GPU kernels
do not silently change results. No production clay material claim is
warranted from distance bonds alone.

### D. Accuracy and performance budgets

- Strict adaptive halo may fall back to global for stiff connected meshes;
  `converged` is only a boundary **heuristic**, not a global-error theorem.
  Track maximum boundary reaction over **all substeps**, not just the last,
  and differential-test 10–100 successive dabs with plasticity/fracture.
- Preserve deterministic serial constraint order as a reference baseline.
  Add graph coloring/threading only after numerical parity tests.
- Future acceleration: generation-marked BFS scratch in the adaptive solver,
  compact/CSR adjacency rebuilt on topology generation rather than nested
  vectors per particle, chunk-based journal if hash-node overhead dominates,
  and dirty-tile surface transfer.
- Benchmark p50/p95 for **complete** brush lifecycle on 10k, 100k, 1M
  *connected* mesh proxies, not just the 125k mostly inactive toy scene.
  Count initial Grab selection, constraint solve, Undo, mesh/CCG writeback,
  PBVH invalidation, RAM/VRAM and GPU draw.
- Use scale-aware tests: inverse masses, rest lengths, stiffness/XPBD alpha,
  extreme brush radii, finite-check exceptions and repeated stroke cancel.
  Physical units and tolerances must be explicit.

## 4. Minimum acceptance criteria for a native Grab gate

1. **Physics OFF / Standard** and `0%` influence must use the original Blender
   brush path with no solver invocation or visual difference.
2. **Physical Assist ON** must use PBVH-selected candidate IDs and produce
   identical single-dab results to reference full-scan selection.
3. A failed step must not write **any** invalid geometry or leave dirty
   viewport tiles, Undo changes or corrupted Multires Sculpt Layers.
4. A correct normal Sculpt workflow must preserve vertex/attribute identity;
   the Multires bridge must preserve high-frequency detail and UV/corner
   attributes through deformations and cancellations.
5. Cross-platform Windows/Linux Blender builds, deterministic tests, and
   actual viewport p50/p95 budgets are required before moving this out
   of the Draft experimental branch.

## 5. Reproduce and Windows guide

The new `test_foundation_contract.cc` suite checks the small inverse-mass
Grab, large representable bond distance, indexed candidate sort/dedup, exact
full-scan parity and invalid-index transactional behavior. The optional
`bench_candidate_query.cc` measures preselected target selection overhead.
See [Windows build/test instructions](WINDOWS_BUILD_AND_TEST_20261119.md).

**Release untouched.** Native Sculpt/PBVH and Multires writeback are **still
not implemented**; this document is the foundation design gate, not a claim
that the integration exists.

## 6. Additional Multires boundary safety regression

A fourth cross-domain correctness defect was reproduced in the standalone
Multires surface bridge: when an anchor uses the fallback for a degenerate rest
triangle, a non-finite deformed proxy coordinate could propagate directly into
the returned evaluated sample without an exception. A non-finite standard
brush coordinate could likewise poison a positive-influence assist blend.

The bridge now rejects non-finite rest anchors/detail residuals and both
non-finite reconstructed and mixed Multires outputs **before returning the
position array**. The original Standard/0%-Assist bypass remains unchanged.
`test_multires_safety.cc` tests both failing input cases and the bypass.

This guarantees *finite output*, not high-frequency detail quality for
degenerate anchors, flipped triangles, or strong shears. Those are still native
Multires/CCG integration gates. With this addition there are **14** CTest
suites on the experimental branch.
