# GuidedQuad architecture and contracts

The core remains independent from Blender. Its input is a neutral triangle mesh, per-face regions, per-vertex scalar fields and, in future, surface-anchored strokes. A Blender standard addon will exchange data via a neutral format and separate executable.

## Pipeline

1. **Guide extraction** — Face Set boundaries, color-region boundaries or painted stroke centerlines (separate modes).
2. **Constraint graph** — source-anchored nodes and curves, hard/soft flags and junction diagnostics.
3. **Cross-field solver** — 4-RoSy tangent orientation with direction controls independent of density.
4. **Density field** — target quad count, curvature and blue/red density weight independently.
5. **Patch layout** — orient quads, place singularities and reconcile integral edge counts; never move hard constraints silently.
6. **Quad optimization** — preserve hard guides and validate high-quality all-quad output before accepting a result.
7. **Projection** — UVs, colors and details, later Multires interoperability.

## Design rules

- Do not merge paint-region boundaries with painted-stroke centerlines; they mean different things.
- Never conflate flow, density and hard loop boundaries.
- When hard constraints conflict with requested quad count, return diagnostics rather than hiding the violation.
- No smoothing of locked input guides; smoothing the field or quad layout is separate.
- CPU baseline, deterministic tests, transparent fallback and benchmark against Exoside on comparable examples.
- The stock Blender addon and Sculpt Extended integration must remain optional adapters.

## V0.1 implementation

Boundary extraction and isocontours from triangles; points anchored to original edges/vertices; loops and paths; fail-fast validation; OBJ output. Does not yet handle true per-corner color samples or quad construction. Face Set region extraction currently follows input triangle edges. These limitations are explicit and will be tested before advancing.

## Benchmark metrics

Hard-guide deviation, Hausdorff-like surface error, quad angular distortion, aspect ratio, singularities and spirals, valid manifold output, runtime, peak RAM. Each future optimization must compare identical mesh, density, and guide settings.
