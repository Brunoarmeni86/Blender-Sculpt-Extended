# GuidedQuad — constraint-first remeshing engine

**Status: experimental V0.1. Code extracts guide paths only; it does not generate quads.**

Independent C++17 project staged on an isolated branch pending migration to a dedicated GitHub repository. No reliance on the Sculpt Extended custom build.

Working: Face Set boundary paths, interpolated scalar contours (suitable for a future Polypaint adapter), exact vertex/edge anchors, open/closed path decomposition, junction/non-manifold diagnostics, OBJ polyline export, CMake/CTest. Original MIT-licensed code.

Build: cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure.

CLI: guidedquad_cli facesets input.obj face_ids.txt guides.obj
CLI: guidedquad_cli contours input.obj scalar_values_per_vertex.txt guides.obj [threshold]

**Still to build:** stock Blender extension and per-corner Polypaint importer, brush-stroke centerline extraction, editable guide constraints, 4-RoSy directional fields, patch solver, full all-quad output, UV/attribute/detail transfer, comparative benchmarks.

Research only (no copied code): Instant Meshes (BSD-style), QuadriFlow (MIT), Geometry Central (MIT), Directional (principally MPL-2.0), QuadWild (GPL-3.0, research only).

Migration: move the contents of experimental/guidedquad/ to the root of a new GuidedQuad repository when that repository becomes available.
