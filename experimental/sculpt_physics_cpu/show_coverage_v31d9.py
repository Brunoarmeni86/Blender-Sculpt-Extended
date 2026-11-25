"""Display a D9 sampled brush dab on a separate mesh copy in Blender.
Run in Object Mode with the original mesh selected; no sculpt data is edited.
"""
import bpy
import csv
import os
from array import array
from collections import Counter
from math import isfinite

def show_coverage(path=None):
    path = path or os.environ.get("BLENDER_SCULPT_COVERAGE_FILE_V31D9")
    if not path:
        raise RuntimeError("Set BLENDER_SCULPT_COVERAGE_FILE_V31D9 before launching Blender")
    source = bpy.context.active_object
    if not source or source.type != 'MESH' or source.mode != 'OBJECT':
        raise RuntimeError("Select the original mesh in Object Mode")
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise RuntimeError("No sampled brush dab in coverage file")
    mesh = source.data
    expected = (len(mesh.vertices), len(mesh.edges), len(mesh.polygons))
    first = rows[0]
    if tuple(int(first[k]) for k in ("verts", "edges", "faces")) != expected:
        raise RuntimeError("Topology count differs from recorded mesh")
    radius = float(first["radius"])
    if not isfinite(radius) or radius <= 0:
        raise RuntimeError("Invalid snapshot radius")
    data = []
    for row in rows:
        i = int(row["id"])
        if not 0 <= i < expected[0]:
            raise RuntimeError("Invalid vertex ID")
        numbers = tuple(float(row[k]) for k in ("host","selected","planned","written","x","y","z"))
        if not all(isfinite(v) for v in numbers):
            raise RuntimeError("Invalid recorded vertex")
        data.append((i,*numbers))
    # Positions may differ from the sampled dab if the user continued
    # dragging. Do not pretend that a later, unrelated mesh is the snapshot.
    from mathutils import Vector
    distance = max((mesh.vertices[i].co - Vector((x,y,z))).length
                   for i,host,selected,planned,written,x,y,z in data)
    if distance > radius * .15:
        raise RuntimeError("Mesh differs significantly from snapshot: make a short stroke and retry")
    copy = mesh.copy()
    obj = bpy.data.objects.new("D9 Coverage - " + source.name, copy)
    bpy.context.collection.objects.link(obj)
    obj.matrix_world = source.matrix_world.copy()
    color = copy.color_attributes.new(name="D9 Coverage", type='FLOAT_COLOR', domain='POINT')
    values = array('f', (.12,.12,.12,1.0)) * expected[0]
    counts = Counter()
    failed = bool(int(first["failed"])) or not bool(int(first["handled"]))
    for i,host,selected,planned,written,x,y,z in data:
        if host <= 0:
            label, rgba = "host zero", (.25,.25,.25,1.)
        elif failed or selected < 0:
            label, rgba = "unknown / failed", (.55,.15,.8,1.)
        elif selected == 0 and planned <= 1e-15:
            label, rgba = "solver excluded", (1.,.04,.03,1.)
        elif planned <= radius*1e-7:
            label, rgba = "no appreciable target", (1.,.32,.025,1.)
        elif written <= radius*1e-7 or written < .25*planned:
            label, rgba = "write attenuation", (1.,.85,.02,1.)
        else:
            label, rgba = "motion written", (.04,.8,.16,1.)
        counts[label] += 1
        values[4*i:4*i+4] = array('f', rgba)
    color.data.foreach_set("color", values)
    copy.color_attributes.active_color = color
    copy.update()
    obj["D9 source CSV"] = path
    obj["D9 action"] = int(first["action"])
    obj["D9 legend"] = "red excluded; orange tiny target; yellow attenuated write; green written; gray native zero; purple unknown"
    obj["D9 sampled rows"] = len(data)
    # Reversible visibility change, only for the two objects.
    source.hide_set(True)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                area.spaces.active.shading.type = 'SOLID'
                area.spaces.active.shading.color_type = 'VERTEX'
    print("D9 coverage:", dict(counts))
    print("Source hidden, not edited. Delete or hide diagnostic copy and unhide source to resume.")
    return obj

if __name__ == "__main__":
    show_coverage()
