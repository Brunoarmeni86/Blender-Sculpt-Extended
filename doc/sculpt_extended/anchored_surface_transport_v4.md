# Anchored Projection V4: Surface Tangent Transport (experimental)

Build on **V3**; keep the original V1, V2 and V3 paths intact. The opt-in property
`use_anchor_surface_transport` uses a free bit 26 of `Brush.flag2` (no Brush struct layout change).

## Pipeline
1. Anchored Stroke, Sculpt Texture = Area Plane, Surface-Adaptive = ON, Surface Path = ON.
2. Optionally enable `Surface Tangent Transport (V4)`.
3. For the **primary symmetry / untiled** pass, V3's bounded CCG Dijkstra search carries
   a surface tangent X axis and two UV coordinates along each accepted shortest-path edge.
4. Transport is the shortest normal-to-normal rotation in the tangent plane, with the axis
   reprojected and renormalized. The next UV increment is the average of old and new
   tangent-basis edge projections.
5. The alpha sample uses the transported UV divided by the current radius.
6. Where transport is not valid (normal inversion/degenerate) use V3 radial path mapping.
   If Dijkstra exceeds the budget, use V1/V2 fallback.

## Limits and correctness
- Up to **100,000 reached samples** for V4, versus 300,000 for the unchanged V3.
- No full-mesh UV arrays, no global mesh update, only stamp-local hash maps.
- Frame map built before parallel brush dispatch and read-only during sampling.
- V4 deliberately disables frame transport in mirrored/radial/tiled passes while maintaining
  V3 projection in those passes. These symmetry cases need independent validation before extension.
- This is an **approximate graph-based discrete tangent transport**, not a distortion-free
  continuous exponential map or UV unwrapping. Different shortest-path trees can introduce seams
  in UV orientation at cut-loci.
- Geodesic computation uses current evaluated CCG positions/normals. Very big stamps, changing
  brush radius, or dense Multires may be expensive, especially during interactive Anchor resize.
- Does not change active Sculpt Layer, Commit, Undo, base cage, or Blender standard mode.

## Tests on the Windows experimental build
1. Backup and compile into a **test build**, not `build_windows_x64_shareable`.
2. Blender factory startup + RNA smoke test.
3. Sphere: compare V1, V2, V3 and V4 with an asymmetric alpha and same radius/rotation.
4. Face (cheek/nose/ear): observe stretching and seams on concave shapes; test Undo and Sculpt Layers.
5. Mirror/radial tests should render as V3; V4 frame transport is deliberately deferred.
6. Test runtime responsiveness with both 30K and 100K grid samples, then larger radius.

**Not verified as Windows compilable or visually correct in this container**. Do not merge into
stable until the user's real Blender compilation/visual tests pass.
