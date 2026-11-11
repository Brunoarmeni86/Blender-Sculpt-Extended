"""Headless tests for the pre-5.4 experimental symmetry/remesh corrections.

Execute with the custom Sculpt Extended Blender binary:
  blender -b --factory-startup --python-exit-code 1 --python doc/sculpt_extended/tests/pre54_symmetry_remesh_regression.py

This script does NOT cover interactive gizmos, the native symmetry passes, Undo,
or the full CDT transition solver. Failing an assert means the candidate must not
be merged. Pass results are meaningful only when executed with Blender Python.
"""
import bpy
import bmesh
import importlib.util
from pathlib import Path
from mathutils import Vector


ROOT = Path(__file__).resolve().parents[3]
MODULES = ROOT / "scripts/startup/sculpt_extended_integrated"


def load(name):
    path = MODULES / (name + ".py")
    spec = importlib.util.spec_from_file_location("sculpt_regression_" + name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_topological_seam_while_posed():
    # A shared, manifold X=0 seam separates two equal-topology halves.
    # The right half is moved far beyond positional mirror tolerance, so
    # a purely nearest-coordinate seed cannot recover its correspondence.
    coords = [
        (-1, -1, 0), (-1, 0, 0), (-1, 1, 0),
        (0, -1, 0), (0, 0, 0), (0, 1, 0),
        (2.8, -0.8, 0), (3.0, 0.25, 0), (2.5, 1.3, 0),
    ]
    faces = [
        (0, 3, 4, 1), (1, 4, 5, 2),
        (3, 6, 7, 4), (4, 7, 8, 5),
    ]
    mesh = bpy.data.meshes.new("__symmetry_seam_fixture")
    mesh.from_pydata(coords, [], faces)
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        mod = load("symmetry_shape_v2")
        mapping = mod.topology_pairs(bm, [Vector(p) for p in coords], .02)
        expected = {0: 6, 1: 7, 2: 8, 3: 3, 4: 4, 5: 5}
        for key, value in expected.items():
            assert mapping[key] == value, (key, mapping, expected)
        left_before = [tuple(bm.verts[i].co) for i in range(3)]
        changed, _ = mod.transfer(bm, bpy.context.scene.cursor.matrix.copy(),
                                  0, -1, .02, False)
        assert changed >= 3, changed
        for i in range(3):
            assert tuple(bm.verts[i].co) == left_before[i]
            a = bm.verts[i].co
            b = bm.verts[i + 6].co
            assert (b - Vector((-a.x, a.y, a.z))).length < 1e-5
        print("PASS: topological seam on strongly deformed/posed halves")
    finally:
        bm.free()
        bpy.data.meshes.remove(mesh)


def test_mask_adaptive_protected_faces():
    # A 7x7 grid with a fully masked center, partially masked collar and
    # unmasked edges exercises discrete remesh with a graded mask field.
    coords = [(float(x), float(y), 0.) for y in range(7) for x in range(7)]
    faces = [(y * 7 + x, y * 7 + x + 1,
              (y + 1) * 7 + x + 1, (y + 1) * 7 + x)
             for y in range(6) for x in range(6)]
    mesh = bpy.data.meshes.new("__masked_remesh_fixture")
    mesh.from_pydata(coords, [], faces)
    attr = mesh.attributes.new(".sculpt_mask", "FLOAT", "POINT")
    for i, (x, y, _) in enumerate(coords):
        d = max(abs(x - 3), abs(y - 3))
        attr.data[i].value = 1.0 if d <= 1 else (0.45 if d <= 2 else 0.0)
    bm = None
    try:
        mod = load("masked_remesh")
        bm, result = mod.local_remesh(mesh, .65, coarsening=2.5, falloff=1.0)
        assert result["protected_topology_exact"], result
        assert result["protected_coordinates_exact"], result
        assert result["protected_faces"] > 0, result
        assert result["boundary_edges"] == 24, result
        assert result["overfull_edges"] == 0, result
        assert len(bm.verts) > 0 and len(bm.faces) > 0
        print("PASS: masked full-face protection with adaptive density")
    finally:
        if bm is not None:
            bm.free()
        bpy.data.meshes.remove(mesh)


def test_mask_extremes_and_multiple_islands():
    mod = load("masked_remesh")

    def grid(name, size, mask_value):
        coords = [(float(x), float(y), 0.0) for y in range(size) for x in range(size)]
        faces = [(y * size + x, y * size + x + 1,
                  (y + 1) * size + x + 1, (y + 1) * size + x)
                 for y in range(size - 1) for x in range(size - 1)]
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata(coords, [], faces)
        attr = mesh.attributes.new(".sculpt_mask", "FLOAT", "POINT")
        for i, co in enumerate(coords):
            attr.data[i].value = mask_value(co)
        return mesh, coords, faces

    # mask=0 is a fully editable surface; mask=1 is bit-for-bit frozen.
    for value in (0.0, 1.0):
        mesh, coords, faces = grid(f"__masked_extreme_{value}", 7,
                                   lambda _co, value=value: value)
        bm = None
        try:
            bm, result = mod.local_remesh(mesh, .65, coarsening=3.0, falloff=1.0)
            if value == 0.0:
                assert result["protected_faces"] == 0, result
                assert len(bm.faces) != len(faces), result
            else:
                assert result["protected_faces"] == len(faces), result
                assert len(bm.verts) == len(coords) and len(bm.faces) == len(faces), result
                assert [tuple(v.co) for v in bm.verts] == coords
            assert result["boundary_edges"] == 24 and result["overfull_edges"] == 0, result
        finally:
            if bm is not None:
                bm.free()
            bpy.data.meshes.remove(mesh)

    # Two separated fully protected islands with a continuous half-mask collar.
    centers = ((3.0, 5.0), (7.0, 5.0))
    def island_mask(co):
        distance = min(max(abs(co[0] - x), abs(co[1] - y)) for x, y in centers)
        return 1.0 if distance <= 1.0 else (0.5 if distance <= 2.0 else 0.0)

    mesh, _coords, _faces = grid("__masked_two_islands", 11, island_mask)
    bm = None
    try:
        bm, result = mod.local_remesh(mesh, .55, coarsening=3.0, falloff=1.25)
        assert result["protected_faces"] >= 8, result
        assert result["protected_topology_exact"] and result["protected_coordinates_exact"], result
        assert result["boundary_edges"] == 40 and result["overfull_edges"] == 0, result
        mask = bm.verts.layers.float[".sculpt_mask"]
        bands = {"low": [], "mid": [], "high": []}
        for edge in bm.edges:
            mean_mask = sum(vertex[mask] for vertex in edge.verts) * 0.5
            key = "low" if mean_mask < .2 else ("mid" if mean_mask < .8 else "high")
            bands[key].append(edge.calc_length())
        means = {key: sum(values) / len(values) for key, values in bands.items() if values}
        assert set(means) == set(bands), means
        assert means["high"] >= means["low"] * .9, means
        print("PASS: mask extremes, continuous collar and multiple protected islands", means)
    finally:
        if bm is not None:
            bm.free()
        bpy.data.meshes.remove(mesh)


def test_mask_closed_manifold():
    mesh = bpy.data.meshes.new("__masked_closed_fixture")
    seed = bmesh.new()
    bm = None
    try:
        bmesh.ops.create_icosphere(seed, subdivisions=3, radius=2.0)
        seed.to_mesh(mesh)
        attr = mesh.attributes.new(".sculpt_mask", "FLOAT", "POINT")
        for vertex in mesh.vertices:
            attr.data[vertex.index].value = 1.0 if vertex.co.z > .8 else (
                .5 if vertex.co.z > -.2 else 0.0)
        mod = load("masked_remesh")
        bm, result = mod.local_remesh(mesh, .42, coarsening=2.5, falloff=1.0)
        assert result["boundary_edges"] == 0, result
        assert result["overfull_edges"] == 0, result
        assert result["protected_faces"] > 0, result
        assert result["protected_topology_exact"] and result["protected_coordinates_exact"], result
        print("PASS: mask-adaptive remesh preserves a closed manifold")
    finally:
        seed.free()
        if bm is not None:
            bm.free()
        bpy.data.meshes.remove(mesh)


def test_existing_nonmanifold_region_is_quarantined():
    size = 9
    coords = [(float(x), float(y), 0.0) for y in range(size) for x in range(size)]
    faces = [(y * size + x, y * size + x + 1,
              (y + 1) * size + x + 1, (y + 1) * size + x)
             for y in range(size - 1) for x in range(size - 1)]
    # A third face on this existing grid edge creates one deliberate overfull
    # region. The remesher must leave its incident vertices untouched while
    # remaining free to update remote manifold geometry.
    a = 4 * size + 4
    b = a + 1
    faces.append((a, b, a + size))
    mesh = bpy.data.meshes.new("__masked_existing_nonmanifold_fixture")
    mesh.from_pydata(coords, [], faces)
    attr = mesh.attributes.new(".sculpt_mask", "FLOAT", "POINT")
    for item in attr.data:
        item.value = 0.0
    bm = None
    try:
        mod = load("masked_remesh")
        bm, result = mod.local_remesh(mesh, .4, coarsening=2.5, falloff=1.0)
        assert result["overfull_edges"] == 2, result
        assert result["boundary_edges"] == 33, result
        assert result["quarantined_faces"] > 0, result
        assert len(bm.faces) != len(faces), result
        print("PASS: existing non-manifold region is quarantined")
    finally:
        if bm is not None:
            bm.free()
        bpy.data.meshes.remove(mesh)


def test_symmetry_mode_transition_state():
    mesh = bpy.data.meshes.new("__symmetry_mode_fixture")
    mesh.from_pydata([(-1, -1, 0), (1, -1, 0), (0, 1, 0)], [], [(0, 1, 2)])
    ob = bpy.data.objects.new("__symmetry_mode_fixture", mesh)
    bpy.context.scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    try:
        bpy.ops.object.mode_set(mode="SCULPT")
        if not hasattr(ob, "sculpt_symmetry_reference_use_for_strokes"):
            print("SKIP: symmetry mode transition requires a build with the native reference backend")
            return
        transition = load("symmetry_reference_preview_transition")
        settings = ob.sculpt_symref_preview
        mesh.use_mirror_x = True
        # Reproduce the reported transition with the legacy channel still on:
        # local symmetry + Combined ON -> Combined OFF. Both channels must be
        # cleared so the following stroke cannot receive duplicate passes.
        ob.sculpt_symmetry_reference_valid = False
        settings.use_for_strokes = True
        ob.sculpt_symmetry_reference_use_for_strokes = True
        transition.ensure_native_reference(ob)
        ob.sculpt_symmetry_combine_local = True
        ob.sculpt_symmetry_combine_local = False
        assert not ob.sculpt_symmetry_reference_use_for_strokes
        assert ob.sculpt_symmetry_reference_valid

        mesh.use_mirror_x = False
        ob.sculpt_symmetry_reference_use_for_strokes = True
        ob.sculpt_symmetry_combine_local = True
        ob.sculpt_symmetry_combine_local = False
        assert ob.sculpt_symmetry_reference_use_for_strokes
        print("PASS: Combined OFF selects native-only or explicit reference-only state")
    finally:
        if ob.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.data.objects.remove(ob, do_unlink=True)
        bpy.data.meshes.remove(mesh)


if __name__ == "__main__":
    test_symmetry_mode_transition_state()
    test_topological_seam_while_posed()
    test_mask_adaptive_protected_faces()
    test_mask_extremes_and_multiple_islands()
    test_mask_closed_manifold()
    test_existing_nonmanifold_region_is_quarantined()
    print("PASS: pre54 symmetry/remesh static-fixture regression")
