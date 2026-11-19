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

## 7. Explicit project objective: integrate open physics libraries, then reach Carnage Grains-like capabilities

**User decision (19 Nov 2026):** Do not limit the plan to a homegrown CUDA rewrite of the existing CPU XPBD brush. **Evaluate actual integration of suitable open-source physics engines/libraries**, and develop comparable *user-visible behavior* to Carnage Grains independently, especially for granular materials, snow/mud/clay, glue/cohesion, plasticity, tearing, caching, meshing and texture transfer. The final project is native Blender Sculpt Extended, **not** merely a Python proof-of-concept or an external simulation viewer.

### 7.1 Updated library shortlist and precise role

| Library / upstream source | Already available publicly | Where it fits | Integration risk / gate |
| --- | --- | --- | --- |
| **Newton Physics** — https://github.com/newton-physics/newton | GPU simulation built on NVIDIA Warp; examples: `mpm_granular`, `mpm_snow_ball`, `mpm_viscous`, `mpm_multi_material`; multiple solvers including XPBD and implicit MPM | **First MPM PoC** for cohesive granular/continuum materials (snow, clay-like, mud); compare actual constitutive models and material controls | Python/Warp execution and dependency packaging in Blender; determine C++/native embedding strategy rather than assume drop-in library |
| **NVIDIA Warp** — https://github.com/NVIDIA/warp | Public GPU/CPU programming framework, CUDA-backed kernels, Apache-2.0 project license | Kernel development layer; ideal for fast independent prototype of custom glue, contact, material painting, and brush-driven forces | JIT/runtime and driver/toolkit packaging; no Python launch/compilation per dab; production embedding viability and startup cost must be measured |
| **Chrono DEM-Engine (DEME)** — https://github.com/projectchrono/DEM-Engine | Public GPU DEM solver supporting grains, clumps, contacts, customizable forces; C++ and Python interfaces, BSD-3-Clause described by project | **Parallel DEM PoC** for loose granular materials, sand, detritus and collisions; compare against MPM and CPU reference | Upstream recommends supported Linux or WSL2 CUDA for Python setup; **Windows native build compatibility must be verified**, not presumed; distinct DEM physics and high integration cost |
| **Blender existing XPBD** — `source/blender/geometry/xpbd/` | Source already in our Blender fork, with C++ constraint API | Retain/share existing CPU XPBD brush semantics and optional soft-glue constraint work | Need adapter and careful ownership; not necessarily an appropriate DEM/MPM engine itself |
| **Carnage Grains** — https://superhivemarket.com/products/carnage-grains | Commercial Blender add-on with advertised glue, attraction, collision/friction, cache/upres, meshing and UV transfer | **Functional reference / behavior benchmark**, not code dependency | Do not import source or replicate branding/UI/assets without permission. Public product feature descriptions do not establish source code availability |

**Do not conflate solvers:** DEM resolves interactions of distinct grains; MPM represents continua/granular-like materials via particles and a grid; XPBD enforces geometric/elastic constraints. A hybrid architecture may offer the right artist-facing presets but may also have coupling, memory and transfer costs. Validate whether a shared simulation state can actually couple these methods instead of promising seamless interoperability.

### 7.2 First executable integration pilot (Codex)

Choose **Newton Physics + Warp as the initial isolated experimental PoC**, since public examples directly target granular, snow and multi-material MPM. Run *outside the main Blender editor* with the supported toolchain, keeping the existing Sculpt XPBD branch and Polypaint R2 untouched.

**Pilot A — technical feasibility, not product integration:**
1. Record OS/Windows version, Python version, NVIDIA GPU model, driver and supported CUDA runtime. Verify the exact Newton/Warp release and license compatibility before install. No unpinned upgrades to Blender's bundled Python.
2. Create a separate, pinned virtual environment under the experimental workspace (not inside `scripts/startup`); install Newton/Warp and run the existing `mpm_granular`, `mpm_snow_ball`, `mpm_multi_material` examples in headless mode where possible.
3. Create our own **neutral test scene**: box, colliders, 10k/100k test particles, deterministic seed, initial particle positions/velocities/material IDs, fixed time step and expected collision boundaries. Compare CPU and CUDA outcomes using identical inputs and documented tolerances.
4. Export/import a **minimal stable interchange** (positions, velocities, particle IDs, material IDs, transforms, simulation time, frame index, topology generation). Test a single Blender frame readback first; measure transfer size, loading time and required synchronization.
5. Prove a **brush-force interface** (grab target, sphere contact/drag, material-specific mask values) without attempting full Blender native Undo. Record startup/JIT time separately from steady-state stepping.
6. Profile end-to-end p50/p95 + VRAM, not only kernel times. Assess whether a persistent in-process backend, native module, or tightly controlled asynchronous worker can be supported **without Python-per-dab overhead or fragile editor dependencies**.
7. Only then propose a Blender build flag / optional runtime backend and a stable native C++ interface; first integration must remain opt-in and have CPU fallback and tested editor Undo/PBVH/Multires transaction gates.

**Pilot B — DEM alternative:** run a comparable set with Chrono DEM-Engine, beginning on the platform upstream supports; compare rolling/sliding, stable grain pile, clumping extensions, memory, contacts and performance. Do not choose DEME solely because it advertises large GPU particle counts.

### 7.3 Carnage Grains-like feature-by-feature parity targets

| Capability to reproduce independently | Candidate mechanics | Blender Sculpt Extended-specific requirement |
| --- | --- | --- |
| Loose grains and collisions | DEM or MPM sand model, granular friction and contact | Paint/select grain spawn region and colliders, reversible editor workflow |
| Glue / breakable bonds / tearing | XPBD glue constraints, fracture energy/yield, cohesive DEM or suitable MPM damage model | Masks/Face Sets material-dependent fracture; topology modifications outside live Multires until safe |
| Attraction / clumping | Neighbour-based cohesive forces, bounded radii and acceleration | Art-directable field, brush influence and material masks |
| Clay, mud, snow, soft flesh | Distinct constitutive/material models; plastic flow, viscosity, compressibility, damping | Physical Assist + Full Physics presets; preserve detail and posed symmetry |
| Animated attach/collider/drag | Persistent handles, robust collision constraints, GPU/CPU synchronization | Native Blender object transforms, modifiers and depsgraph |
| Baking and playback | Versioned disk cache with stable IDs and material metadata | Save/reopen/replay; invalidate on source topology/material change |
| Upres without rerun | Particle flow interpolation / secondary detail conditioned on cached coarse motion | Adaptive microdetail and Multires detail transport; no global subdivision |
| Surface/strand meshing | Anisotropic particle reconstruction, implicit surface / strand mesh from glue graph | UV/color-per-corner provenance, newly exposed interior mapping, editable output where feasible |
| Texture transfer | Reference surface anchoring and barycentric interpolation | Preserve Polypaint and UV seams, don't silently average corner discontinuities |

**Acceptance is behavior, not identical implementation:** For each item, define scene fixtures, rendered/mesh outputs, numerical metrics, stability and artist usability comparisons. Target functionality only; no copying original source, assets, packaging or trademarked presentation.

### 7.4 Immediate task order

- **First:** finish compiling/testing the present V12 native CPU Grab on the current Polypaint source; record the actual full Blender build and ensure no Polypaint regression.
- **Then, in parallel on an isolated workspace:** run the Newton/Warp MPM smoke test and DEME comparison. **Do not insert an untested dependency into the fork.**
- **Next:** define a reusable `GranularBackend` / `PhysicsBackend` neutral particle/material interchange contract and benchmark host-device transfers and persistent state.
- **After that:** bring a first CUDA-backed simulation into Blender as an opt-in **simulation mode/object cache** (safer than immediately mutating live Multires), then expose brush-driven force inputs.
- **Finally:** integrate volume/material simulations into normal Sculpt and Multires with transactionally validated proxy-to-surface transfer and native Undo; expand into glue/fracture/upres/meshing.

**Decision checkpoint:** prefer Newton+Warp **for first proof of concept**, not as a permanent dependency selection. The actual backend must be selected by correctness, performance, maintenance, platform and deployment tests, with Chrono DEM-Engine retained as a specialist granular contender. Do not equate Cycles CUDA/OptiX with any of this implementation.
