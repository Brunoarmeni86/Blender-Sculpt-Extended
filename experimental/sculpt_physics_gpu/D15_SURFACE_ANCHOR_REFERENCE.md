# D15-C1/C2: UV reprojection and source-triangle anchor reference

Independent C++17 CPU reference, no SculptCore source imported. Complements existing physics_core.hh SurfaceAnchor and SculptSurfaceBridge (which evaluate previously assigned particle anchors and detail residuals); does not replace them. D15 feature Smooth and existing UI unchanged.

API takes immutable local triangles, stable triangle and vertex IDs, source component and UV chart IDs, generation, per-corner UVs for ONE layer, explicit adjacent triangle pairs, seed anchor and query position/normal. Closest point on closed triangle and barycentric sample. Bounded traversal only from valid seed, same component/chart, shared geometric edge with consistent endpoint positions and UVs, normal alignment and maximum hop/distance. No global nearest fallback. Sort candidates by distance then stable triangle ID; equivalent edge ties accepted, conflicting near ties rejected. Malformed input, missing/stale seeds, degenerate triangles or no safe hit leave output anchor unchanged. Distances/ambiguity tolerance in caller units; cap 4096 triangles, 16384 links, 32 hops. Query defaults illustrative, not calibrated for artist use.

Limits: caller must supply valid local manifold adjacency and component/chart IDs, keep vertex IDs/triangle corner ordering stable within generation, validate source snapshot and build native wedge groups; not a manifold validator. Hop bound is not a geodesic distance bound. Normal test plus topology prevents disconnected shortcuts but cannot guarantee avoiding every connected fold, especially similarly oriented sheets; distinguish from exact surface tracking. Only one UV map per query; all-layer/wedge batch transfer and coherent commit required later. No automatic UV chart construction, no attribute writes, no arbitrary attribute interpolation API, no Blender native Undo integration, no material-particle provenance, no new interior UV creation. Source anchors locate attributes, not physical target positions; never snap newly simulated geometry back to old surface.

Validation actually executed in Linux container on standalone source:
- g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 test_d15_surface_anchor.cc -o test_gcc && ./test_gcc
- clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -O1 -g test_d15_surface_anchor.cc -o test_clang && ./test_clang

Both printed D15_SURFACE_ANCHOR_REFERENCE_OK with no sanitizer diagnostic. Fixtures cover interpolation, stale generation, adjacent triangle walk, shared-edge tie, chart/UV/component barriers, hop budget, opposite normal, closer disconnected sheet, ambiguous overlap, clamped closest point, degenerate triangle, duplicate IDs, bad link, NaN and retained output on failed match. CMake target registered; full suite, Windows native build, Blender runtime and performance untested.

Next gates:
1. Native corner-domain UV Undo audit, including geometry+attribute atomicity; no UV publication before this.
2. Build local snapshot/triangle adjacency and separate per-layer corner wedge charts; stable IDs scoped to topology generation, bounded physical travel, explicit rejection on unsupported topology. Stage accepted geometry and UV outputs together, keep previously pinned splits until validated; initial tangential-relax-only option.
3. Reuse source triangle anchor design for volumetric attribute transfer, but extend with persistent material coordinates and source provenance, not geometric snap. New surfaces need explicit interior attribute policy.
4. Dense mesh tests, checker texture, seam/UDIM, folds, masks, multiple UV maps, Undo/Redo, no regression when off; then consider GPU batching. 
