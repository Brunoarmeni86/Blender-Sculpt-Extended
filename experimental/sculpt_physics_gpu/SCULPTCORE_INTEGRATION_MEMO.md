# SculptCore research and integration memo — D15 / future Blender 5.4 port

Date: 2026-11-27. Audience: Codex and Sculpt Extended maintainers.
Target: experiment/physics-d15-volumetric-only, baseline 6d3adfa25db95cdd350f431e69c134e3eed4e5f1.
Decision record, not authorization to implement every item together.

## 0. Non-negotiable direction

- Keep working Smooth variants, in particular recovered Classic Very Strong. No regression in default modes, Preserve Shape, native clipping, masks, symmetry or Undo. New modes are opt-in.
- Keep one native BSE Clay Strips preset. Do NOT revive D12-D14 artificial material deposition, ClayBulb, Median Fusion, Continuous Fusion or V15 surface brush dispatch. D14 archive: experiment/physics-d13-dedicated-sculpt-engines at f6f0ba6b2abb14e14cb18ab20f778efc406e4b36.
- Real physical brushes will solve a volumetric material problem (mass/momentum, constitutive law, contact, possible topology change), not a vertex displacement presented as MPM. SculptCore supplies candidate geometry infrastructure, not a substitute for the volumetric solver.
- Preserve Blender's PBVH, Mesh/BMesh/CCG, attribute domains, native editor and native Undo. Do not introduce a second authoritative mesh, independent sculpt Undo, a second GPU runtime, or a full SculptCore import just to reuse small algorithms.
- Keep compatibility with existing Sculpt Extended work (layers, posed symmetry, Multires; future LOD, Microdetail, Nanomesh). Audit current source before modifying anything. Do not assume latest public Blender has the same implementation as this fork.
- GuidedQuad remains a separate project. Global quad remeshing is not an interactive volumetric surface extraction algorithm.

## 1. Evidence and license

Upstream: https://github.com/joeedh/sculptcore (Joseph Eagar), branch master as read on date above. Inspected actual source, not only documentation. License file https://github.com/joeedh/sculptcore/blob/master/LICENSE.txt is MIT; preserve copyright/license for copied substantial portions; audit per-file provenance and external dependencies separately. Some upstream comments explicitly describe ports of Blender: do not infer MIT relicensing of third-party GPL code from repository-level license. Prefer native Blender implementation when equivalent, otherwise a narrow independently integrated port with clear attribution. Record exact upstream commit and per-file license before copying. The blob IDs below identify inspected file contents, NOT commit IDs; no immutable upstream commit was resolved in this review. Fetch master, pin commit and recheck before implementation. No compile, benchmark, visual evaluation or parity runs conducted here. No claim that upstream features are production-ready. No verified MPM/PBF in the examined SculptCore sources.

### Source index (confirmed files; URLs relative to https://github.com/joeedh/sculptcore/blob/master/)

| Path | Inspected blob SHA / significance |
|---|---|
| source/brush/kernels/bsmooth.sbrush | 067a606f3cb197189dcdfb36b9eab9d6dacadbc3 — boundary-aware stencil |
| source/mesh/boundary.h | classification, dirty cache |
| source/mesh/uv_reproject.h | corner UV reprojection contract |
| source/mesh/uv_reproject.cc | 4845e2ddcdd5efde74faec34f2e49f0bee6e6ecd — staged UV wedge projection |
| source/brush/grid_gpu_session.h | 203cbce8d99ad9507aecf306af6f239b2fcbde54 — resident grid session |
| source/brush/stroke_spacing.h | residual arc-length sampler |
| source/brush/plane_frame.h | explicit Blender-inspired frame policy |
| source/brush/kernels/plane.sbrush | one-sided plane projection |
| source/brush/kernels/kelvinlet.sbrush | analytic linear elastic displacement, NOT volumetric |
| source/dyntopo/dyntopo.h | d0afec4d1fdfc55e671db19947b87d097cec02b3 — adaptive split/collapse/flip |
| source/mesh/utils/edge_collapse.h | 85756166cda471b2c4162913663548346d18f58e — link/geometric checks, attribute transfer |
| source/mesh/utils/pinch_off.h | 4800e44385b928d5e6e94745abe9c62e58c85972 — separating 3-cycle cut/caps |
| source/mesh/attr_merge.h | 2015ab5f5e426ca1d8f6514b46ca72e92521d210 — attribute merge policy |
| source/remesh/remesh.h | 2e30e918a24834b797951679d87fad08d2dd5a1b — MIQ-style global pipeline |
| source/remesh/remesh_params.h | 75ab54c0c7a3610dab5f4eeb7cc80ad0361131d8 — options and tradeoffs |
| source/remesh/remesh_report.h | cb1e36ead879242215c2f1d957f1b13a884cc2d9 — diagnostics |
| source/remesh/field/constraints.cc | 7ce2d902c4e31bc6626e5bf493ab79380457caa4 — hard/soft face-direction constraints |
| source/remesh/extract/reproject.cc | 125361debda1552b6e829827681d2ecf2bf78fa9 — source anchors and sheet filtering |
| source/subdiv/CMakeLists.txt | a4c937da6eb02ee327fc8439f4b70fb3073ecf69 — grid domain/log, layers/levels/propagation |
| source/displace/CMakeLists.txt | 06c45d785f03ed3b943331656e5760242e8c6739 — compositor and frames |
| source/brush/CMakeLists.txt | staged execution, stroke batch interfaces, DSL compiler and GPU marshal |

The DSL design document documentation/brush_compute_dsl.md has an older design-only status; actual CMake and .sbrush files show implemented components. Do not infer all proposed targets (CUDA/HIP/OpenCL etc.) work. The boundary kernel explicitly comments CPU-only attribute dispatch for its vclass field despite @gpu annotation. Grid session describes WGSL/SPIR-V. Inspect current generated kernels and backend tests before claiming parity. Missing README.md and source/remesh/c-api/remesh_c_api.h returned 404; use actual listed remesh headers/implementation instead.

## 2. First independent improvement: feature-aware Smooth (P1; after baseline validation)

Upstream: boundary.h and bsmooth.sbrush. Separate persistent explicit edge flags (sharp/projected/seam) from derived polygroup and UV-chart boundaries, classify vertices and dirty-update. Smooth splits neighbor displacement into normal/tangent components, restricts tangential neighbors by boundary type; sharp creases suppress normal motion; sharp junctions pin; endpoints/nonsharp junctions usually suppress tangent sliding. Important nuance: kernel endpoint handling overrides earlier normal weight; not all junctions pin. Type-bit overlap is not the same as membership of the identical feature chain; improve this explicitly if needed. Tangential projection reduces shrinkage but is not exact volume preservation.

Native audit: source/blender/editors/sculpt_paint/mesh/sculpt_boundary.hh already exposes ensure_boundary_info, create_boundary_info, vert_is_boundary; smooth.cc uses boundary_info_cache and boundary-aware native neighborhood selection. Do NOT replace them indiscriminately. Inspect sharp_edge, seam, Face Set and actual corner UV discontinuities, hidden faces, PBVH grid boundaries and existing Preserve Shape. Add optional feature flags/edge-chain IDs to existing compact CSR, with invalidation on topology and attribute change. Maintain deterministic node order, dedup shared vertices, separate read/write Jacobi buffers. CPU oracle first; then dedicated Warp kernel/optional neighbor weights. Preserve existing Classic and all mode mappings, exact accepted-position synchronization and session reset. Do not alter native smooth fallback. Prefer a new opt-in mode such as Feature Preserve; name subject to existing UI audit.

Acceptance: identical legacy output when off; tilted plane, cube crease, endpoint, sharp junction, UV seam, Face Set junction, disconnected close sheets, mask/hidden vertices, irregular valence and boundary edit invalidation; CPU/GPU tolerance documented. Measure crease drift, corner drift, roughness, area/volume proxy and frame p50/p95 including transfers on realistic dense meshes. Verify Undo/Redo, clipping and multi-dab state. No claim of speedup until measured.

## 3. UV and attribute transport (P1/P2, separate option)

Upstream uv_reproject.cc: snapshot old positions, per-corner UV wedge grouping by actual old UV equality, closest point on old incident fan, barycentric interpolation, staged writes, corner change callback before publishing. Not a global texture reproject or arbitrary high-deformation attribute advection. Do not cross UV chart, source component or opposite sheet; corner-domain UV values can be discontinuous even at one vertex. Inspect tolerance and degenerate-triangle fallback. Old-fan closest point can be wrong for large motion; expand only within a bounded source chart or reject. Multiple UV layers and arbitrary attributes require explicit per-domain policy.

Blender plan: for pure tangential relax only, snapshot impacted faces and old vertex positions before accepted coordinate write. Transfer UVs after computing accepted positions, not requested ones. Native corner attributes + Undo capture together; do not write if no safe attribute journal. Support face attributes as discrete (Face Sets/material IDs), point colors/masks as continuous where valid; do not interpolate indices, flags, weights without semantic rules. For volumetric reconstruction, use material/source-face anchors instead of pretending old one-ring exists for new topology. Newly exposed interior has no original UV: define explicit policy/new material; never fabricate copied texture continuity.

Acceptance: split UV island seam, coincident UV charts, multiple maps, textured checker plane under tangent relax, no-op/zero-motion, masks and hidden faces, degenerate triangles, order independence, Undo restores both geometry and UVs. Track UV stretch and visible texture drift. Off by default. Keep Photogrammetry addon separate but document possible reuse.

## 4. GPU Multires Smooth (P2; high priority, independent of volume)

Upstream grid_gpu_session.h: dense grid domain, ring1 CSR, GPU state resident across dabs, capture affected leaves before any position readback, per-dab moved-vert readback for hit/bounds, touched ID set and timing. Kernel normals remain stroke-static while CPU normals refresh; not acceptable by assumption for strong deformation. Full upload at begin, transfer budget and per-dab synchronization need measurement; no measured advantage for our RTX 3080. source/subdiv/CMakeLists.txt names grid_domain, grid_stroke_log, multires_layers/levels/propagate and channel APIs; these are entry points for follow-up audit, not fully reviewed implementations.

Native plan: adapt CCG/PBVH Grids to compact canonical sample IDs including shared edge/corner samples, lattice adjacency and correct cross-grid ownership. Avoid full mesh conversion and do not duplicate shared seam vertices. Use existing GPU Smooth backend/session, native factor calculation and layer-delta logic; upload dirty blocks, sync accepted positions, refresh normals as needed, correct object/local displacement conversion, native bounds/normals propagation. Topology/level generation invalidates state; level switch/Undo abort or flush. Keep ordinary mesh path unchanged. Initially one opt-in mode with ordinary Multires and no active sculpt layer; expand only after tests for layer isolation and propagation. Native CPU fallback if unsupported BEFORE any write. Do not accidentally edit base cage or another layer.

Acceptance: CPU/CUDA parity on irregular subdivision grids and boundaries; no seams/cracks, no double write at shared corner, mask, hidden regions, layer isolation, change level down/up, Undo/Redo, accepted-position correction, normals refresh; record memory and per-dab p95 at increasing grid counts. Existing supported CPU Multires must remain unchanged.

## 5. Dyntopo and local reconstruction (P2 research / P3 implementation)

Upstream dyntopo.h: CPU triangle remesh with length bands, graded radial sizing, optional per-vertex sizing, independent-edge selection, split/collapse budgets (zero unlimited), shorter-diagonal convex flips, optional bounded tangential relax and UV reproject, protected feature edges. GradedRecursive explicitly ports Blender's pbvh_bmesh scheme; existing native pbvh_bmesh.cc has recursive edge length growth and boundary rules. Audit actual differences before port. Independent sets are not proof that topology mutations run GPU-parallel. Candidate scoring GPU and conflict-free selection CPU may be useful; keep topology/Undo CPU until safe. Upstream claims about split count and frame time are comments, not benchmark results here.

Edge collapse: topological link condition, normal/fold checks, capture old corner attributes, UV continuity constraints. Audit implementations of split/flip/merge before copying. Pinch-off: pinchSeparatingTriangle cuts a non-face separating 3-cycle in oriented closed manifold, duplicates ring and caps both sides with opposite orientation, can refuse before mutation. This is a local topology surgery for thin necks, not brittle fracture physics, arbitrary cutting, fusion or validated volume conservation. Source attributes copied to caps are not a physical interior-material model. Use optional surgery only after manifold/geometry checks, and native BMesh transaction integration. No default automatic pinch-off.

For volumetric brushes: closed surface -> local signed distance/occupancy and particle seeding -> persistent MPM/PBF state -> GPU scalar field -> extraction with stable interface -> protected native splice -> native Undo. Dyntopo is an optional surface quality step after extraction, never the material solver. Test collar stitching, unchanged exterior, no holes/T-junctions, face orientation, nonmanifold edges, collision rejection, split/merge, stable feature/attribute IDs, rollback on failure. Forbid unsupported open surfaces or require explicit thickness policy. Keep topology changes away from active Multires until a real detail transfer strategy exists.

## 6. Source anchors and thin sheets (P1 design, P3 integration)

Upstream remesh/extract/reproject.cc implements source-face anchors propagated from work mesh, local source-face walks, fallback to nearest surface with normal compatibility when coherent. Normal filter is heuristic; code may fall back to global nearest when anchors fail, or reject filtered hit when too far. Not a proof of sheet preservation. For local physical reconstruction, maintain source face ID + barycentric coordinates + component/chart ID and generation, use material particle IDs where possible. Prefer anchor tracking over global nearest; enforce hard component/chart constraints and explicit reject when ambiguous. Never project true new surfaces to old surface (would undo physical shape change). Use anchors to transfer attributes and high-frequency residuals, not to overwrite volumetric displacement. Test two almost-touching parallel sheets, folds, thin ears, opposite normals, disconnected components, lost anchors and extreme distortion. Keep native collision and topology guards; not exact CCD.

## 7. GuidedQuad opportunity (independent backlog)

Upstream remesh.h pipeline: curvature/features -> cross field -> singularity adjustment -> seamless parameterization -> quantization -> quad extraction -> reprojection. Source/remesh/CMakeLists.txt lists Eigen and optional CHOLMOD. Native CLI target exists. RemeshParams has target count or edge length, sharp thresholds with hysteresis/min chain, density, triage, pre-remesh, optional source anchors, odd-rim caps, speed/quality policies. Odd-rim capping can introduce triangles; do not label strict all-quad without validating. Field constraints use 4-RoSy complex exp(i4phi), hard user direction .remesh.f.stroke_dir, sharp/open border constraints on nonflat faces, soft anisotropy-weighted curvature, ambient comb on flat caps to avoid cones. Face direction alignment is NOT equivalent to guaranteed edge loop placement along a specific user curve. Crossing/conflicting constraints need explicit arbitration. Reports include failures, folds, singularities and stage status; mirror this reporting rather than blindly returning a mesh.

Prototype separate GuidedQuad backend (native CLI or C API only after actual ABI/license review); input face directions from geometric guides, Face Set boundaries, PolyPaint boundaries or curve strokes; preserve guide provenance independently of materials. Test closed creature/character, flat caps, thin folded ears, scanned nonmanifold data, open boundaries, disconnected pieces, multiple guide intersections, target counts and density. Compare existing GuidedQuad/Exoside (if licensed and available) on guide alignment, deviation, quad ratio, pole placement, count error, aspect ratio, manifold, time and failure reporting. No claim of superiority without benchmark. Do not import global MIQ pipeline into each sculpt dab.

## 8. What NOT to port now

- Plane frame and plane.sbrush largely replicate Blender. Native Clay Strips already has its own plane/tip and spacing; tune one asset with visual comparison to ClayBuildup, no claim of exact clone. CPU frame policy uses live or stroke-start references; GPU host frame differs. Do not use plane target again for true material accumulation.
- StrokeSpacer carries residual between polyline segments. Useful reference for path-dose sampling in volumetric tool contact, but Blender already resamples strokes. Audit first; interpolate pressure/orientation/radius and avoid double spacing or double pressure. Stationary dwell explicitly defined by tool/time, not event frequency. Test subdivided trajectory.
- Kelvinlet is analytic linear elastic field, can be useful as an optional control or validation fixture but not the new volumetric Flesh solver; do not reintroduce old surface brush roster.
- sbrush DSL, whole mesh storage, separate rendering, complete GPU dispatch architecture and global remesher are not immediate dependencies. Keep Warp. Revisit only when measured need.
- Multires layers/VDM architecture: inspect multires_layers.cc, multires_propagate.cc, compositor.cc, frames.cc and tests in future. Build file listing alone does not establish semantic parity; preserve current Sculpt Extended layers. Future microdetail/Nanomesh requires detail-carrier transfer independent of base topology, not a copied layer system.

## 9. Execution sequence for Codex (small PRs, no blind implementation)

A. Baseline D15 build/asset smoke FIRST: WITH_BSE_VOLUMETRIC_ONLY=ON, V14/V15=OFF, shared Warp ON, run existing Smooth reference tests and native interactive A/B; confirm correct PTX, Classic log, asset library exactly one custom Clay Strips, no stale experimental dispatch. Existing D15 compile status unverified in this review. If build fails, fix smallest cause, don't replace functioning Smooth.
B. Independent branch/PR for feature-aware Smooth CPU oracle + native boundary cache audit. Only after pass, add GPU kernel and parity/performance. Keep off by default. C. UV transfer opt-in with native Undo; D. Grids GPU Smooth prototype. These are geometry track, can proceed independently from physics track after A.
E. Physics track: inspect D14_GPU_VOLUME.md, prototype_warp_volume_mpm_d14.py, prototype_warp_volume_pbf_d14.py, test_warp_volume_d14.py. Current MPM PIC dense grid/small strain; PBF hash grid, both first CUDA fixtures passed in prior user log, CPU compiler unavailable, not integrated in Blender. Implement MLS-MPM/APIC affine velocity and finite deformation with objective constitutive/plastic law; conservative transfer and rollback; independent CPU oracle, mass/momentum (external force/contact accounted), rigid translation/rotation, shear/compression, yield, timestep/CFL and long-run tests. Then sparse grid, actual swept tool contact, material persistence, GPU extraction, source-anchor transfer, native splice transaction. Keep simulation compute and surface editor interface separate. Do not enable a brush until complete geometry result can be committed safely.
F. Dyntopo topology-quality research and source-anchor mapping in isolated tests before native splice. Fracture/fusion/thermal material behavior after core; no synthetic surface substitute. GuidedQuad study separate.
G. Migration to future Blender 5.4: keep patches scoped, record upstream merge base and conflicts, rerun baseline. Do not migrate merely to resolve unrelated implementation issues.

Every PR: inspect source and tests; report changed paths, commit SHA, exact test commands/output, measured performance and unsupported modes. Distinguish reference tests, CUDA tests, build, Blender runtime and artist validation. No percentages of completion without defined gates. No silent fallback after mutation, no fabricated tests, no destructive history rewrites. User preference: quality sculpting first; preserve working Smooth and abandon old artificial physical brushes.

## 10. Present status

Research memo only. No SculptCore code imported, no CMake or runtime change, no tests executed. Priorities are recommendations based on the verified source evidence and D15 design decision, not a completed roadmap. Next action is A (baseline) then E in parallel with small opt-in geometry improvements, subject to resource and dependency review.

## Implementation checkpoint — feature-aware Smooth CPU oracle

See [D15_FEATURE_SMOOTH_REFERENCE.md](D15_FEATURE_SMOOTH_REFERENCE.md). Independent CPU reference and synthetic regression tests now exist; GCC and Clang ASan/UBSan standalone tests passed. No native dispatcher or UI change, no CUDA parity or full build yet. Baseline D15 remains a prerequisite to native integration. This is not a resurrected surface physical brush.
