"""Stress, attribute, detail and thin/concave tests for protected remesh."""

import importlib.util
import ctypes
import math
import time
from pathlib import Path

import bmesh
import bpy
from mathutils.bvhtree import BVHTree


ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "scripts" / "startup" / "sculpt_extended_integrated" / "masked_remesh.py"
spec = importlib.util.spec_from_file_location("masked_remesh_stress_module", MODULE)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


def working_set():
    counters = PROCESS_MEMORY_COUNTERS()
    counters.cb = ctypes.sizeof(counters)
    current_process = ctypes.windll.kernel32.GetCurrentProcess
    current_process.restype = ctypes.c_void_p
    get_memory = ctypes.windll.psapi.GetProcessMemoryInfo
    get_memory.argtypes = (ctypes.c_void_p, ctypes.POINTER(PROCESS_MEMORY_COUNTERS),
                           ctypes.c_ulong)
    handle = current_process()
    assert get_memory(handle, ctypes.byref(counters), counters.cb)
    return counters.WorkingSetSize


def source_tree(mesh):
    bm = bmesh.new()
    bm.from_mesh(mesh)
    tree = BVHTree.FromBMesh(bm)
    bm.free()
    return tree


def topology_counts(bm):
    return (sum(edge.is_boundary for edge in bm.edges),
            sum(len(edge.link_faces) > 2 for edge in bm.edges))


def maximum_surface_error(bm, tree):
    maximum = 0.0
    for vertex in bm.verts:
        nearest = tree.find_nearest(vertex.co)
        assert nearest[0] is not None
        maximum = max(maximum, nearest[3])
    return maximum


def maximum_triangle_edge_ratio(bm):
    maximum = 1.0
    for face in bm.faces:
        lengths = [edge.calc_length() for edge in face.edges]
        if lengths and min(lengths) > 1e-12:
            maximum = max(maximum, max(lengths) / min(lengths))
    return maximum


def large_mesh_test():
    mesh = bpy.data.meshes.new("__protected_remesh_large")
    seed = bmesh.new()
    result = None
    try:
        bmesh.ops.create_icosphere(seed, subdivisions=6, radius=2.0)
        seed.to_mesh(mesh)
        initial = (len(mesh.vertices), len(mesh.polygons))
        mask = mesh.attributes.new(".sculpt_mask", "FLOAT", "POINT")
        weight = mesh.attributes.new("probe_weight", "FLOAT", "POINT")
        face_sets = mesh.attributes.new(".sculpt_face_set", "INT", "FACE")
        for vertex in mesh.vertices:
            mask.data[vertex.index].value = max(0.0, min(1.0, (vertex.co.z + .7) / 1.4))
            weight.data[vertex.index].value = 2.0 + vertex.co.x * .2 + vertex.co.y * .1
        for polygon in mesh.polygons:
            face_sets.data[polygon.index].value = 1 if polygon.center.z >= 0.0 else 2

        tree = source_tree(mesh)
        memory_before = working_set()
        started = time.perf_counter()
        result, info = mod.local_remesh(mesh, .12, coarsening=4.0, falloff=1.0)
        elapsed = time.perf_counter() - started
        memory_after = working_set()

        final = (len(result.verts), len(result.faces))
        assert final != initial
        assert topology_counts(result) == (0, 0), info
        assert result.verts.layers.float.get("probe_weight") is not None
        assert result.faces.layers.int.get(".sculpt_face_set") is not None
        values = [vertex[result.verts.layers.float["probe_weight"]] for vertex in result.verts]
        assert values and all(math.isfinite(value) for value in values)
        assert set(face[result.faces.layers.int[".sculpt_face_set"]] for face in result.faces) <= {1, 2}
        error = maximum_surface_error(result, tree)
        ratio = maximum_triangle_edge_ratio(result)
        assert error < 2e-4, error
        assert ratio < 12.0, ratio
        print("LARGE_REMESH", {"initial": initial, "final": final,
              "seconds": elapsed,
              "working_set_delta_mib": (memory_after-memory_before) / (1024 * 1024),
              "surface_error": error, "max_edge_ratio": ratio}, flush=True)
    finally:
        seed.free()
        if result is not None:
            result.free()
        bpy.data.meshes.remove(mesh)


def thin_concave_test():
    bpy.ops.mesh.primitive_torus_add(major_radius=2.0, minor_radius=.18,
                                    major_segments=192, minor_segments=24)
    obj = bpy.context.object
    mesh = obj.data
    result = None
    try:
        mask = mesh.attributes.new(".sculpt_mask", "FLOAT", "POINT")
        for vertex in mesh.vertices:
            mask.data[vertex.index].value = max(0.0, min(1.0, (vertex.co.x + .6) / 1.2))
        tree = source_tree(mesh)
        result, info = mod.local_remesh(mesh, .09, coarsening=4.0, falloff=1.0)
        assert topology_counts(result) == (0, 0), info
        error = maximum_surface_error(result, tree)
        ratio = maximum_triangle_edge_ratio(result)
        assert error < 2e-4, error
        assert ratio < 12.0, ratio
        print("THIN_CONCAVE", {"final": (len(result.verts), len(result.faces)),
              "surface_error": error, "max_edge_ratio": ratio}, flush=True)
    finally:
        if result is not None:
            result.free()
        bpy.data.objects.remove(obj, do_unlink=True)


large_mesh_test()
thin_concave_test()
print("PASS: protected remesh stress, attributes, detail and thin concave fixtures", flush=True)
