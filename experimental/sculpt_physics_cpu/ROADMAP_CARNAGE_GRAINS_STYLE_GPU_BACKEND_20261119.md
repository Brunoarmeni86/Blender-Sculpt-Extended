# Sculpt Extended Physics — Carnage Grains Capability Reference and GPU Roadmap

**19 November 2026 | Engineering memo for future implementation | Status: PROPOSED, not implemented.**
**Branch:** `experiment/sculpt-physics-native-grab-v12-20261119`; Draft PR #23.
**Baseline:** the V11 C++17 CPU reference in Draft PR #21, and the opt-in V12 *native Grab prototype*.

## 0. Source attribution and implementation status: very important

**Carnage Grains (Max Miles / MilesFX) is a feature and behavior reference, not imported source code.** It publicly advertises GPU-powered grain/soft-material simulation and **NVIDIA CUDA** requirements. We have **NOT** obtained, cloned, embedded or implemented its proprietary/internal engine, kernels, CUDA algorithms, add-on code or assets. We must not claim to have done so. Do not assume public source availability or a license authorizing source reuse; verify ownership/licensing before any proposed dependency.

Our `experimental/sculpt_physics_cpu` is an *independently implemented CPU-only* XPBD reference, not a Carnage Grains port. The V12 opt-in normal-Sculpt Grab adapter is an early native integration attempt and has **not** been certified by a complete Blender editor build, sculpt viewport/Undo test or production performance data. The standalone CPU CI does not prove native functionality. The prior Polypaint R2 build has CUDA/OptiX enabled for **Cycles rendering**, **not** the physics solver.

**Useful documented references:** https://superhivemarket.com/products/carnage-grains ; https://milesfx.xyz/docs/getting-started ; https://github.com/NVIDIA/warp ; https://github.com/newton-physics/newton . NVIDIA Warp and Newton are *separate open-source projects*, not the Carnage Grains codebase. If an external library is integrated later, review its actual license and dependency impact at that time.

## 1. What to aim for (capability parity, not source copying)

Move from a single XPBD Grab experiment toward a **general physics-assisted sculpting and granular deformation system**:
- **Clay / mud / wax:** elastic + plastic response, volume-preserving deformation, adhesion/cohesion, smoothing, material presets. The artist should still be able to change brushes mid-stroke/workflow without switching Blender modes.
- **Grains / sand / snow:** dynamically moving particles with broadphase, local neighbor contact, friction, collisions, cohesion and settlement. Do not use pure distance-bond XPBD as a substitute for a granular contact solver.
- **Soft tissue / flesh:** stretch, shear, bend/volume constraints, controlled tearing and optionally connected/cohesive material clusters. Tearing and topology change must be opt-in and confined to a separate safe geometry workflow before attempting live Multires edits.
- **Brush family:** Grab first; then Clay/Clay Strips, Smooth physical/coarse relaxation, Inflate/Move, and later material-specific cutting/scraping. `Standard`, `Physical Assist`, `Full Physics` modes with truly zero-cost/no-change bypass for Standard or zero assist.
- **Material painting:** masks, Face Sets and Polypaint can *optionally* select material physical properties (stiffness, density, damping, plasticity, cohesion, friction, yield) through a distinct material attribute; painted color must not silently be reinterpreted as physics.
- **High-detail sculpting:** physically simulate an adaptive **low/mid-resolution proxy** and transfer deformation to a high-resolution surface without damaging Multires Sculpt Layers, UV/color corners, detail, shape keys or the current pose/symmetry system.

These are progressively testable feature targets; none of the listed advanced simulation materials is validated in Blender yet.

## 2. Structural architecture to freeze before implementing GPU

```text
Blender Sculpt input / PBVH & CCG query / brush factors / symmetry / material paint
                   |
          Brush Adapter + topology generation
                   |
         Persistent PhysicsSession per object/stroke
        particle proxy IDs, canonical materials,
        constraint state, stable anchors, active islands
                   |
      Backend-independent physics state & typed passes
       stretch | bend | shear | volume | contact |
       friction | plasticity | cohesion | fracture
                   |
          Backend dispatch per island/step
          CPU reference / CPU parallel / GPU compute
                   |
        Staged validated results + native Undo transaction
        -> dirty PBVH / Multires tiles / attributes & viewport
```

**Mandatory contracts:**
1. Editor owns brush factors/masks, Undo/Redo, PBVH, symmetry and Sculpt Layer policy. Solver owns physical state/constraints and returns only *staged* changed proxy IDs + positions. No Python-per-dab simulation.
2. **Stable IDs and generation counters** for mesh topology, Multires level, Sculpt Layers, proxy graph and surface anchors. Invalidate cache on revision changes; never silently apply a simulation result to a remapped vertex.
3. **Persistent proxy**: V12 currently reconstructs the graph on **every dab** and limits simulation to directly targeted nodes with `bond_hops=0`. Replace this before scaling. Reuse adjacency, lambdas/velocity if the material model requires continuity, and dirty-island/halo buffers across dabs.
4. **Canonical CPU reference** remains deterministic regression oracle. Evaluate sharing/factoring the fork's existing `source/blender/geometry/xpbd/` implementation rather than permanently maintaining two divergent solvers.
5. **Contiguous data layout**: SoA or carefully packed typed arrays for positions/velocities, inverse masses, rest lengths, lambdas, material IDs and flags, plus CSR/compact neighbor tables. Design once for CPU vectorization and GPU upload.
6. **All-or-nothing editor commit**: numerical finite validation, bounded displacements, Undo coverage, stable topology and material sanity **before** writing. Rejected GPU steps must not corrupt the CPU source state or native Undo.
7. **Multires detail preservation**: coarse proxy + per-sample/corner anchors, explicit detail residual and stable UV/color corner provenance. Transfer into the currently active Sculpt Layer only; nonactive/hidden layers untouched. Freeze/save/load/reopen and Undo must pass before enabling.
8. Solver clock and parameters are explicit: fixed time step, XPBD compliance scaling with dt, substeps, iteration count, bounded recovery on abrupt mouse motion, scale-aware tolerances, deterministic constraint ordering baseline.

## 3. CPU maturity and test gates — prerequisites for CUDA

**P0: complete current Blender-native V12 build and reliability**
- Preserve the **19 November Polypaint R2** source/successful executable as the baseline; do not reset or overwrite Codex's uncommitted work.
- Validate the opt-in V12 code compiles and links in a *separate* full Blender build, and the runtime flag actually activates Grab. Check original Grab parity with flag OFF; test mask, hidden vertices, Face Sets, PBVH node boundary, Undo/Redo/cancel, repeated dabs and nonlinear moves.
- No claim that standalone CPU suite establishes native editor correctness.

**P1: persistent scalable CPU session**
- Build persistent solver graph once per stable topology generation, not per dab.
- PBVH-driven candidate IDs, local connected-component islands with configurable halo, tracked maximum boundary reaction over substeps, adaptive re-solve when necessary.
- Fast path for sub-20k preview, then staged 100k, 1M proxy cases. Avoid building a graph at *display resolution* (adaptive proxy density independent from Multires global level).
- Add CPU parallelism (constraint graph coloring / safe Jacobi-like pass), differential tests against deterministic serial reference.

**P2: credible materials and contact**
- CPU distance/stretch, bending, shear, volume/incompressibility, plastic deformation and explicit material masks.
- Build separate spatial-hash / BVH collision broadphase for contact; Blender PBVH is good for brush *selection*, not automatically a complete granular neighbor/contact solution.
- Static/dynamic colliders, friction, cohesion, grain size and rest contact; independently test pile/snow and stretch/tear scenes.
- Only then promote clay, granular and soft tissue behaviors as genuinely physical.

**P3: native Multires and brush expansion**
- Build validated proxy-to-CCG transfer compatible with Sculpt Layers and preserving fine displacement, face-corner UV and color attributes; stable topology IDs.
- Add assist to Clay/Clay Strips, Smooth and other brushes through a shared brush-target API, not solver duplication in each brush.
- Explicit topological symmetry/pivot integration, shape key/modifier limitations, Undo and save-load tests.

## 4. NVIDIA CUDA / GPU strategy — planned, NOT yet coded

**Target design:** `Backend = Auto | CPU | CUDA` initially. Consider Vulkan/Metal only after profiling, portability and implementation feasibility. **CUDA/OptiX being available in Cycles does not confer any CUDA physics functionality.** Never make a compatible NVIDIA card mandatory for ordinary Blender sculpt.

Evaluate two implementation avenues independently:

- **Native CUDA C++ kernels:** more direct control of persistent device buffers, compilation, scheduling and integration, but substantial engineering and toolchain/deployment cost. Kernel stages: update brush targets -> integrate -> neighbor/binning broadphase -> solve constraints/contact -> update material/velocity -> produce compact dirty span -> staged readback.
- **NVIDIA Warp research prototype:** public-source Python JIT kernels for GPU numerical simulation; useful to validate kernels and performance quickly. **Not** the same as Carnage Grains. Python/JIT startup, distribution, licensing, build and embedding must be assessed before using Warp inside Blender; do not make Python invocation per dab the production solution.
- **NVIDIA Newton:** separate publicly available engine primarily oriented to robotics; review solver semantics and suitability, but do not assume its simulation data models are appropriate for clay/grains or Blender Multires.

**GPU engineering contracts:**
- **GPU-resident state** across dabs: positions/velocities, constraints, contacts and material fields persist in VRAM. Upload only changed brush targets/parameters or updated topology regions; read back only dirty result spans or a bounded proxy region. CPU remains authoritative for Blender geometry and native Undo.
- **Minimize synchronization:** avoid host↔device memcpy or blocking stream synchronization per particle/per constraint. Batch updates and, if safe, use double-buffered staged results with explicit version/ownership.
- **Constraint conflict handling:** graph-colored Gauss-Seidel or bounded Jacobi iterations; quantify convergence and divergent numerical behavior, avoid unchecked floating-point atomics and avoid claiming bitwise identical CPU/GPU outputs.
- **Device capabilities and fallback:** detect driver, CUDA toolkit/architecture support, available VRAM and device failures. Keep CPU-only builds, fallback after out-of-memory, and all existing Blender brush behavior available.
- **Build isolation:** CUDA optional at compile/runtime; avoid unconditional CUDA headers in common editor and solver interfaces. Document Windows/MSVC/CUDA and supported GPUs before distributing binaries; opt-out default until validation.
- **Telemetry:** record kernel, H2D/D2H, synchronization, CPU adapter, Undo, PBVH/CCG update, viewport draw, VRAM peak and context startup individually. No “GPU faster” claim from kernel-only benchmarks.

## 5. Benchmarks and hard acceptance gates

**Standard fixtures:** fixed mesh 10k, 100k, 1M *connected* proxy vertices (hardware permitting); 10–100 successive brush dabs; material stress: stiff clay, yielding mud, grain pile, self-contact, bending tissue, fracture event; versions with Multires fine detail and UV/color seams.

Measure:
- Complete dab p50/p95/p99 latency and worst spike, including brush candidate query, cache, solver, staging, mesh/CCG writeback, Undo and viewport update;
- GPU kernel-only vs **end-to-end** speedups, VRAM/RAM, host-device traffic and initialization overhead;
- displacement residual, constraint error, volume loss, energy/momentum where relevant, plastic strain drift, boundary reaction, penetration and collision stability;
- no-physics bypass overhead, frame/paint responsiveness, thread safety, cancel/rejected dab and crash recovery;
- full Windows build smoke tests (editor linked), Linux if available, CPU-only fallback and representative NVIDIA hardware.

**Go/no-go:** do not merge a GPU backend until it outperforms the optimized CPU backend on representative stroke workloads *end-to-end* (not a synthetic kernel benchmark), preserves agreed numerical tolerances and Undo/geometry invariants, and does not regress CPU-only Blender.

## 6. Prioritized next tasks for Codex

- [ ] Capture current full **Polypaint R2 + V12** source snapshot, source SHA, all uncommitted modifications, build flags, compiler logs and runtime activation evidence.
- [ ] Fix native editor compile / integration defects; validate Undo, PBVH, mask and exact Standard-bypass in actual Blender viewport.
- [ ] Extract reusable stroke/session and transaction API; avoid solver-specific code growing inside `grab.cc`.
- [ ] Refactor reference solver topology/constraint buffers into reusable SoA/CSR and deterministic reference execution.
- [ ] Add persistent proxy and sparse PBVH-selected island stepping without O(N+E) rebuilds per dab.
- [ ] Implement volume, bend/shear and material plasticity; contact broadphase and friction; regression fixtures.
- [ ] Create CUDA feasibility benchmark against optimized CPU on exactly the same state/constraints; investigate NVIDIA Warp **only as a separate prototype**.
- [ ] Integrate device-resident compute behind a backend interface with CPU fallback and transactional dirty readback.
- [ ] Extend brushes and Multires / Sculpt Layers once physical behavior, Undo, and detail transfer are reliable.
- [ ] Track per-stage benchmark and gates in documentation before increasing particle counts or enabling by default.

**Summary:** Learn from Carnage Grains' *demonstrated capability category* (GPU-driven grains, cohesive/soft materials, tearing), not from unverified internal source. Preserve the existing Blender/Polypaint toolchain, finish a reliable native CPU integration, then move to an independently engineered, optional CUDA backend.
