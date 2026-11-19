# Native Physics Engine, Physics-Assisted Brushes and Multires Mask Extract
**Design proposal — 18 November 2026 | Status: PLANNED / NOT IMPLEMENTED**

This document specifies a possible native C++ physics subsystem for **Blender Sculpt Extended**, inspired by the *capability category* of particle-based granular/deformable simulators such as Carnage Grains. It does not claim integration of, access to, or derivation from Carnage Grains source code. Blender Sculpt Extended is an independent, experimental Blender fork.

## Goals

1. Reusable **native simulation engine** for granular, cohesive, elastic and plastic materials: sand, mud, clay, snow, soft tissue, breakable bonds and later fragmentation.
2. Shared **physics-assisted regular sculpt brushes** (not a separate sculpt mode): Normal / Physical Assist / Full Physics; initial targets **Grab, Clay Strips and Smooth**. Artists must still be able to use all ordinary brushes without invoking the solver.
3. **Multires + Sculpt Layers compatibility**: preserve active/nonactive layers, protected masks, high-frequency detail, symmetry, Undo and save/reload. No forced switch to Object Mode for brush interaction.
4. Surface correspondence and **UV/Color Attribute persistence** through deformation, resampling and topology changes, with explicit handling of newly exposed fracture interiors.
5. **Multires Mask Extract** with two distinct outputs: high-fidelity frozen/evaluated detail, and later a truly editable Multires hierarchy.
6. Multi-backend architecture: CPU baseline first; accelerated compute backends are optional, capability-tested implementations, not requirements for Blender to start.

## Existing source to reuse or audit, not rewrite

- XPBD solver node: `source/blender/nodes/geometry/nodes/node_geo_xpbd_solver.cc`. It exposes substeps, constraint iterations and geometry bundles. The currently available implementation is a **research baseline**, not proof of an accessible general-purpose standalone solver or GPU granular support.
- Core XPBD constraints: `source/blender/geometry/` (audit actual constraint APIs, ownership, thread safety, collision support and ability to run outside Geometry Nodes before refactoring).
- Existing Mask/Face Set extraction operator: `source/blender/editors/sculpt_paint/mesh/sculpt_mask_extract.cc`. Its existing path copies the base Mesh into BMesh, removes unselected faces and clears `.sculpt_mask` and `.sculpt_face_set` on output; it does **not** by itself implement a Multires-grid-preserving extraction.
- High-resolution Sculpt Layers / attributes: `source/blender/blenkernel/BKE_multires_sculpt_layers.hh` and `source/blender/blenkernel/intern/multires_sculpt_layers.cc`. Reuse existing stable grid identities, local commit and active-layer conventions.
- PBVH updates and brushes: `source/blender/editors/sculpt_paint/mesh/`; existing `smooth.cc`, Sculpt Undo code and `Detail Transport Copy V01` should be studied for compatibility.

**Baseline note:** repository `main`, release checkpoint and experimental branches can differ significantly. Inspect the exact target branch/commit before code integration. Do not modify the November 4 approved release tag or claim any new binaries.

## Proposed architecture (not existing paths)

```text
Blender Sculpt / Simulation Nodes / Modifier / Timeline
                 |
    BKE Physics Session + stable data contracts
                 |
    Native Physics Core (C++)
    - particles, constraints, material regions
    - XPBD, integration, spatial hash, contacts
    - plastic strain / bond rest updates / fracture
    - surface correspondences / texture transfer
                 |
   +-------------+-------------+
   | CPU multi-thread baseline | GPU compute optional
   |                           | CUDA, later Vulkan / Metal
   +-------------+-------------+
                 |
 Brush Adapter / Scene Simulation Adapter / Mesher
                 |
    PBVH dirty regions / Multires layer transaction
```

Avoid a Python-per-dab simulation loop. GPU kernels must keep dynamic particle data resident where possible; CPU-only and CUDA must obey the same invariant tests and produce tolerance-equivalent results. The viewport renderer supporting Vulkan/Metal does not automatically imply a compute solver on those APIs.

### First-pass internal interfaces (pseudocode, not compilable Blender code)

```cpp
enum class PhysicsBackend { Auto, CPU, CUDA, Vulkan, Metal };
enum class BrushPhysicsMode { Standard, PhysicalAssist, FullPhysics };

struct PhysicsMaterial {
  float density;
  float compliance;
  float damping;
  float friction;
  float cohesion;
  float yield_strain;
  float plastic_creep;
  float fracture_strain;
};

struct PhysicsStepInput {
  float dt;
  int substeps;
  int iterations;
  // Brush-local force / displacement targets or scene forces.
};

class PhysicsWorld {
 public:
  virtual ~PhysicsWorld() = default;
  virtual bool initialize(/* particles, bonds, colliders, materials */) = 0;
  virtual void apply_brush_targets(/* stable sample ids + target data */) = 0;
  virtual void step(const PhysicsStepInput &) = 0;
  virtual void gather_changed_regions(/* dirty particle ids / surface anchors */) = 0;
};

// API contract for a separate Sculpt->Physics adapter, not a brush rewrite:
struct BrushPhysicsSettings {
  BrushPhysicsMode mode = BrushPhysicsMode::Standard;
  float influence = 0.0f; // Physical Assist blend; 0 = identical to standard.
  bool preserve_volume = false;
};
```

**Critical invariant:** `Standard` or `influence == 0` must reproduce the existing brush path, including protected layers, symmetry and Undo, without allocating an active physics world.

## Brush behavior

- **Standard**: ordinary Blender brush unchanged.
- **Physical Assist**: the brush computes an artistic target displacement; the solver adds optional constrained material response (plasticity, cohesion, elastic return, volume response). A deterministic blend creates the final per-stroke deformation.
- **Full Physics**: brush interaction generates forces, constraints or moving contact targets; physics determines the response. The artist can pause/freeze the response.
- First three tests: **Grab** (elastic deformation), **Clay Strips** (plastic displacement/accumulation) and **Smooth** (coarse physical/geometric relaxation that retains fine Multires detail).
- Paint material properties via separate typed attributes, optionally derived from Polypaint / Face Sets / masks; do not conflate painted visual color with physical material IDs by default.
- Preserve topology symmetry and chosen pivot during posed sculpting where existing remapping allows it; complex posed symmetry is a separate incomplete feature.

## Multires and data preservation

Use a **simulation proxy independent of display/sculpt resolution**, plus per-sample anchors to source surface and, when appropriate, stable coarse-grid IDs. For unchanged topology, use the original per-corner UVs/Color Attributes and map only positions. For remeshed or fractured surfaces, use source face/corner provenance plus barycentric interpolation or constrained closest-surface correspondence. Preserve seams, material IDs and attribute discontinuities, and generate separate UVs for exposed interiors. Mark missing or ambiguous provenance instead of claiming perfect transfer.

Sculpt Layers should receive **active-layer displacement deltas** through a transaction, retaining nonactive/hidden layers and high-frequency detail. Support coherent stroke begin / dab / commit / cancel / Undo and robust CCG/PBVH invalidation. Fractures change topology and must initially be restricted to the simulation-object workflow, not applied destructively to a live Multires hierarchy.

## Multires Mask Extract (parallel workstream)

- **High Fidelity Extract (first)**: sample/extract the evaluated Multires surface at the selected resolution using the **HR mask**, interpolate the boundary, generate an independent dense mesh; preserve visible sculpt detail, UVs, material assignments, polypaint and other compatible attributes. Original remains unchanged. Output is **not** an editable Multires modifier hierarchy.
- **Editable Multires Extract (second)**: generate a valid new low-resolution control cage; reconstruct levels and resample/transport fine detail. Preserve Sculpt Layers only where a trustworthy new-grid remap exists; otherwise issue clear warnings and offer flatten/bake with source retained.
- Additional options: Extract as Solid, thickness, boundary smoothing/projection, mask threshold, optional new-object selection. Handle seam corners and discontinuities.
- Avoid assuming that existing **Face Set HR Extract** already solves **Mask Extract HR**: the former is configuration/feature-flag limited and is not equivalent.
- Dedicated tests: mask crosses UV seam; open edge; multiple islands; masked hidden regions; high layer strength; L0/L1/highest; UV and color preservation; Undo; save/reopen.

## Sequenced implementation and test gates

**P0 — Audit/prototype API**: trace XPBD data ownership and stepping in the target branch; choose CPU core ownership boundary; document particle/bond/cache schema; instrument baseline brush latency and mask extraction output.

**P1 — CPU physics prototype**: isolated volume proxy + spatial hashing + XPBD distance/contact constraints, deterministic fixed-timestep stepping, brush-local interactions; Grab Physical Assist first. Test sphere/clay lump (10k–50k particles); explicitly measure brush p50/p95 time, memory and volume error rather than presuming interactivity.

**P1b — Multires Mask Extract High Fidelity**: independently implement and validate HR mask extraction, source-to-output corner attribute transfer and reliable Undo, before attempting editable new grids.

**P2 — Physical Assist expansion**: Clay Strips plasticity, Smooth coarse-scale relaxation, preserve volume, masked/Face-Set material response, Sculpt Layer integration.

**P3 — Carnage-like simulation features**: attraction, persistent Glue bonds, fracture limits, granular contacts, robust broadphase, cache/timeline, marching-cubes/surface-nets experiments; UV/corner-attribute correspondence and fracture-interior mapping.

**P4 — Backend acceleration and editable extraction**: profile CPU vs CUDA; add GPU only after stable reference tests. Vulkan/Metal implementations are later feasibility gates. Rebuild editable Multires hierarchy from a new cage and transfer detail with validated errors.

### Acceptance tests

- OFF or 0% Assist matches standard brush output within predeclared tolerance, with **no regression in default brush timings**.
- On CPU-only systems, all standard brushes work and the optional physics feature degrades gracefully.
- Every physical brush stroke can **Undo/Redo** cleanly; cancel and save/load work, and hidden/protected layers remain bitwise unchanged where required.
- UV/Color Attributes survive continuous deformation; seams and fractures have explicit error/coverage reports.
- At high Multires levels, proxy-derived broad deformation does not erase fine sculpted detail.
- Benchmark particle counts (10k, 50k, 100k, 500k where hardware permits), contact-heavy scenes, cached replay, p50/p95 per dab, synchronization stalls, peak RAM/VRAM and full save/reopen.
- No new behavior is claimed in a released build until the exact branch, SHA, build, visual results and test fixtures are documented.

## Open technical decisions

1. Reuse XPBD solver internals or factor shared constraints into a standalone native C++ library? Decide after code ownership and extensibility audit.
2. How are proxy surface anchors updated for extreme plastic deformation, self-contact and tearing?
3. How to preserve UV island boundaries when a new surface is reconstructed?
4. Should Full Physics permit remeshing during a stroke, or remain a simulation-object workflow initially? **Initial recommendation: no Multires topology changes mid-stroke.**
5. CPU deterministic reference vs accelerated approximate kernels, acceptable tolerances and hardware test matrix.

### External background (not dependencies)

- Blender's own XPBD Geometry Nodes implementation, in this source tree.
- NVIDIA Warp: https://github.com/NVIDIA/warp (CPU and NVIDIA CUDA research option, not AMD/Intel GPU support).
- Carnage Grains public documentation: https://milesfx.xyz/docs/getting-started (behavioral reference only; no source integration assumed).

**Status at document creation: design/specification only. No source implementation or validated performance claim.**
