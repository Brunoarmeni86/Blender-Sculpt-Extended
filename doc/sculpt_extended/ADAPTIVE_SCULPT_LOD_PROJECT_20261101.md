# Blender Sculpt Extended — Adaptive Sculpt LOD Project

**Status:** Proposed / architectural research  
**Date:** 2026-11-01  
**Scope:** Sculpt Extended Multires, Sculpt Layers, PBVH and SubdivCCG  
**Implementation status:** Not implemented. This document defines the intended design and validation path.

## 1. Objective

Allow different parts of a Multires object to be sculpted and displayed at different resolutions without subdividing or changing the topology of the base mesh.

The base cage, UVs, weights, Face Sets and modifier topology remain stable. Resolution changes affect the displacement-grid evaluation domain, the resident CCG data and the PBVH working set.

The system must support:

- manually assigned sculpt resolution;
- automatic refinement around the brush;
- persistent, locked resolution regions;
- regions defined from Face Sets or masks;
- multiple named LOD profiles in the same `.blend`;
- local Commit, Preserve Other Layers, masks, symmetry and Undo;
- conservative fallback to the existing uniform Multires path.

## 2. Motivation and measured baseline

The current CCG uses one global grid level. A small edit can therefore require a surface containing every grid at the selected level.

Observed on the current reference object:

| Evaluated level | Grid area | Total samples | Approximate CCG CPU memory |
|---|---:|---:|---:|
| 5 | 289 | 122,667,784 | 1,404 MiB |
| 4 | 81 | 34,380,936 | 393 MiB |
| 3 | 25 | 10,611,400 | approximately 120 MiB |

Dropping one level reduces sample work by roughly 3.6 times on this object. An adaptive working set can preserve level 5 only where it contributes visible or sculptable detail.

The existing local lower-level Commit demonstrates the core principle: persistent high-resolution layer data can remain intact while only the edited coarse samples are processed. Adaptive LOD extends this principle to CCG evaluation, PBVH construction and viewport drawing.

## 3. Design principles

1. **The base mesh is never subdivided by LOD.** Resolution belongs to displacement grids.
2. **Persistent detail is not discarded by coarsening.** A lower active LOD changes residency and evaluation, not the stored master detail.
3. **Resolution is explicit and inspectable.** Artists can see, assign, lock and restore it.
4. **Local operations stay local.** Brush promotion, Commit, normals and PBVH updates operate on changed tiles plus a conservative dependency halo.
5. **Every optimized route has a uniform fallback.** Unsupported topology, modifiers or data domains use the established path.
6. **Mixed-resolution seams are deterministic.** Shared physical samples have one canonical value and order-independent publication.

## 4. User model

### 4.1 LOD profiles

A mesh can contain multiple named profiles, for example:

- `General Sculpt`;
- `Face Details`;
- `Pose Performance`;
- `Laptop`;
- `Final Inspection`.

Changing profile changes the evaluated working resolution. It does not resample or delete stored Multires and Sculpt Layer detail.

Each profile contains ordered region groups. A group defines:

- name and stable identifier;
- source binding;
- minimum, maximum or exact level;
- lock policy;
- brush-promotion policy;
- safety-halo width;
- overlay color;
- optional viewport-only level cap.

### 4.2 Region sources

The same LOD group can be populated from:

- one Face Set;
- a collection of Face Set IDs;
- the current sculpt mask;
- selected faces;
- a persistent region painted directly with a Resolution brush.

Face Sets and masks are selectors, not the only storage for resolution.

### 4.3 Binding modes

**Live Face Set binding**

Membership follows the referenced Face Set IDs. Moving a face to another Face Set updates its LOD group.

**Snapshot binding**

Current membership is copied into a persistent LOD-region attribute. Later Face Set or mask edits do not change it.

**Mask capture**

Mask values are converted into one region or quantized into several level bands. The live sculpt mask can then be cleared without losing the LOD assignment.

### 4.4 Lock policies

| Policy | Meaning |
|---|---|
| Automatic | Runtime may refine or coarsen inside the profile limits |
| Minimum Locked | Runtime may refine, but cannot go below the assigned level |
| Exact Locked | Runtime must keep the assigned level |

`Minimum Locked` is the preferred policy for important features: it protects eyes, lips, hands and other detail while allowing temporary refinement under the brush.

### 4.5 Effective-level rule

For an automatic group:

```text
effective_level = clamp(runtime_requested_level, group_minimum, group_maximum)
```

For an exact lock:

```text
group_minimum = group_maximum = assigned_level
```

When multiple rules affect a face, explicit exact locks have priority; otherwise the highest required minimum wins. Conflicting exact locks must be rejected by validation rather than resolved implicitly.

## 5. Persistent data model

The first implementation should use face-domain region membership because it is stable, easy to visualize and compatible with existing Face Set workflows.

Proposed persistent elements:

- a face-domain integer attribute for snapshot region membership;
- named LOD profiles stored with the Multires modifier;
- groups containing stable IDs and Face Set bindings;
- per-group level, lock and promotion settings;
- active-profile identifier;
- schema version for file migration.

The exact DNA representation must be chosen only after save/load, duplication, library override and topology-remap tests. Temporary runtime pointers, CCG offsets and PBVH ownership never belong in persistent DNA.

Later versions may move from per-face resolution to per-grid or per-grid quadtree tiles while retaining the same profile and group interface.

## 6. Runtime representation

### 6.1 Phase A — profile infrastructure

Implement profile storage, region assignment, locking, overlay and Undo while continuing to evaluate one uniform level. This validates the artist-facing model without changing CCG topology.

### 6.2 Phase B — grouped uniform evaluation

Use the active profile to choose a global interactive level and allow temporary full-level inspection. This provides a safe performance mode and validates profile switching.

### 6.3 Phase C — per-face/per-grid mixed resolution

Each base face or grid receives an effective level. Runtime storage records:

- effective level;
- grid size and sample offset;
- resident state;
- dirty domains;
- boundary ownership;
- parent/master-detail relationship.

This phase requires removing the assumption that all `SubdivCCG` grids share one `grid_size` and one fixed offset formula.

### 6.4 Phase D — adaptive grid tiles

Divide a grid into power-of-two quadtree tiles. Only tiles near the brush, silhouette, camera or visible high-frequency detail are promoted. Child detail remains stored when a tile is evaluated through its parent.

This is the target “true adaptive LOD” architecture.

## 7. Mixed-resolution seams

Adjacent regions can differ by at most one evaluated level in the first mixed-resolution implementation. Wider differences are bridged by automatically inserted transition rings.

Boundary rules:

- coarse and fine grids share canonical samples at matching dyadic coordinates;
- fine-only edge samples are constrained by the coarse edge representation;
- a brush crossing a boundary promotes the adjacent tile and at least one halo ring;
- normals include both sides of the boundary;
- masks, Face Sets and layer deltas use the same physical-sample ownership;
- publication is transactional and deterministic across duplicate grid coordinates.

Viewport skirts are not acceptable for sculpting because they hide cracks without making geometry, raycast and brush coordinates coherent.

## 8. Sculpt behavior

### 8.1 Brush promotion

Before a stroke modifies geometry:

1. determine tiles intersecting the brush support;
2. include symmetry copies and duplicate boundary samples;
3. expand by the configured halo;
4. promote tiles to the required sculpt level;
5. rebuild only affected PBVH leaves;
6. begin Undo capture after promotion is complete.

Promotion must be synchronous for the immediate brush region. Optional prefetch can prepare nearby tiles, but correctness cannot depend on it.

### 8.2 Coarsening

Coarsening changes active evaluation only. Stored child detail remains available. Automatic coarsening is forbidden while a stroke, filter, transform, Commit or Undo transaction owns the affected region.

### 8.3 Sculpt Layers and Commit

Sculpt Layers remain stored at their master level. Lower-resolution edits use nested Multires sample coordinates and publish corrections only to dirty regions. The existing local lower-level Commit is the reference behavior.

### 8.4 Preserve Other Layers

Preserve samples other layer grids at exact nested coordinates for the active tile level. It must not flatten or resample their high-frequency data. The current lower-level Preserve stride model is the reference for mixed-resolution tiles.

## 9. Undo, save and locking

Profile selection, group membership, level changes and lock changes must be undoable independently from sculpt strokes.

Undo records persistent profile/region changes and affected tile state, without copying the entire CCG. Runtime tile residency is reconstructed from persistent profile data when necessary.

Save/load requirements:

- profiles and locks survive reopening the `.blend`;
- missing Face Set IDs are reported and do not silently reassign faces;
- snapshot regions remain valid when the source mask is cleared;
- older files without LOD data load with the uniform legacy profile;
- disabling adaptive LOD produces the same persistent sculpt result through uniform evaluation.

## 10. User interface

Proposed Sculpt Extended panel:

- active LOD profile selector;
- profile duplicate, rename and delete;
- region-group list;
- `Assign from Active Face Set`;
- `Assign from Face Set Group`;
- `Capture from Mask`;
- `Paint Resolution`;
- `Set Minimum`, `Set Exact`, `Unlock`;
- automatic-promotion and halo controls;
- colored level overlay;
- locked-region hatch or border;
- estimated resident samples and memory.

The overlay must distinguish stored master detail from currently evaluated resolution.

## 11. Compatibility and fallback

Initial mixed-resolution support should require:

- one compatible Multires modifier;
- unchanged base topology;
- supported Sculpt Layer descriptors;
- no unsupported live deformer between the cage and Multires;
- valid power-of-two level relationships.

Unsupported operations use a uniform CCG at the required level. Examples may include export, bake, topology-changing operations, unsupported filters and legacy add-ons that assume one global grid size.

Fallback must preserve data and produce a diagnostic reason.

## 12. Performance targets

Performance is measured against sample count and affected tiles, not only frame rate.

Targets for the reference object:

- avoid materializing all 122.7 million level-5 samples for local sculpting;
- keep normal interactive resident memory below 500 MiB when less than 20% of the model needs level 5;
- promote a brush region without a full-object CCG rebuild;
- keep local Commit proportional to dirty samples;
- rebuild PBVH leaves only for changed or transitioned tiles;
- make profile switching proportional to tiles whose effective level changes.

These are targets, not current measured results.

## 13. Validation matrix

Every milestone must cover:

- quad and non-quad base faces;
- poles and extraordinary vertices;
- adjacent regions at different levels;
- brush strokes crossing LOD boundaries;
- symmetry across mixed levels;
- masks and hidden regions;
- Face Set edits after live and snapshot bindings;
- multiple visible Sculpt Layers;
- Preserve Other Layers and Delta Smooth;
- lower-level local Commit;
- Undo/Redo across strokes, profile changes and level changes;
- save/reopen;
- modifier duplication and object duplication;
- uniform fallback comparison;
- finite-value and seam checks;
- memory-budget pressure.

Numerical comparisons must sample boundary positions, derivatives and normals against a uniform master-level evaluation.

## 14. Delivery milestones

1. **Project data and UI:** profiles, groups, Face Set collections, mask snapshots, locks and overlay.
2. **Safe global profile LOD:** one runtime level selected from a profile; no mixed grids.
3. **Per-face/per-grid prototype:** mixed levels with one-level adjacency and conservative halos.
4. **Local PBVH and normal updates:** eliminate full rebuilds for tile promotion.
5. **Layer/Undo integration:** local Commit, Preserve, masks, symmetry and targeted Undo.
6. **Quadtree tiles:** refinement inside grids and automatic brush promotion.
7. **Stability gate:** fallback audit, corruption tests, long-session tests and clean builds.

No milestone is integrated into the Stable build until its validation matrix passes.

## 15. Non-goals for the first implementation

- replacing Dyntopo;
- changing base-mesh topology;
- deleting high-resolution detail when a region is coarsened;
- arbitrary non-power-of-two resolutions;
- invisible viewport-only crack hiding;
- GPU-only data that cannot be reconstructed or validated on CPU.

## 16. Open decisions

- whether persistent snapshot membership belongs in a generic mesh attribute or modifier-owned data;
- how live Face Set bindings behave when IDs are removed or merged;
- maximum permitted neighboring level difference after the prototype;
- whether viewport level and sculpt level are separate group properties;
- RAM-budget policy for resident tiles and preserved child data;
- how external modifiers and Python APIs expose variable grid sizes;
- whether profile data is shared across linked duplicates or kept per modifier instance.

These decisions must be resolved through small prototypes before changing the public file format.
