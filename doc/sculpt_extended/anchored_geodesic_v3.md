# Anchored Projection V3 - Surface Path (experimental)

Source branch: `feature/anchored-geodesic-v3`, based on the validated V2 branch.

## User workflow
* Stroke Method: Anchored
* Primary Sculpt Texture (alpha): Area Plane
* Surface-Adaptive Projection: enabled
* Normal-Guided (Multires): optional V2 compensation
* Surface Path (Multires, Experimental): enabled for V3

## Actual implementation
* For each CCG sculpt action, compute a local shortest-path field from the nearest visible sample under the stamp center.
* Search a weighted graph of CCG vertices using Dijkstra: 8-neighbors inside grids, topology-aware seam adjacency and duplicate seam samples across grids.
* Edge cost uses true 3D CCG segment length. Graph distances approximate geodesic radius, not a continuous exact geodesic.
* The alpha's radial coordinate follows the path length, while its angle remains in the anchored brush's tangent plane: not a full curved-surface 2D unwrap.
* Samples unreachable at the stamp radius are masked (prevents stamping straight through nearby disconnected geometry), provided the path cache was successfully computed.
* Only one stamp-local cache, up to 300,000 samples; zero global dense arrays. The cache is computed **before parallel brush workers** and read-only thereafter.
* Geometry larger than the cap (or invalid graph/missing data) uses V1/V2 instead; Standard still operates as before.
* Radius update and mirror/radial/tile symmetry influence cache validation and regeneration.

## Limitations
* Runtime and Visual Studio 2022 build unverified when committed. DO NOT merge without tests.
* A graph shortest path is longer than or equal to the continuous geodesic; the finite 8-neighbor lattice causes residual directional bias.
* Projection angle is planar, so large folds may still shear/wrap; a true 2D tangent transport/exp-map is a future step.
* Currently used in common Multires brush factor path (not every specialized brush).
* Rebuilding the cache as the Anchored radius grows can be costly on large stamps; profile before use on 60M+ meshes.
* Does not alter native active Sculpt Layer Undo/Commit; verify they still work.

## Windows smoke test
1. Apply the V3 incremental patch on top of your working V2 sources; compile in the test build (do not overwrite shareable stable).
2. Startup factory; same displacement alpha on a sphere, test Standard -> V1 -> V2 -> V3.
3. Fold a connected sheet so both sides are close in world space but far along the surface, check V3 does not stamp through.
4. Test a face with neck/ear curvature, then enable Sculpt Layers and test Undo/Redo.
5. Test mirrored strokes, then low-to-high Multires levels, performance and stalls.
6. Report first C++ error and log if build fails; ExitCode=1 is only the wrapper.
