"""Dense ordinary-mesh probe for Sculpt Extended Physical Balloon.

Run with a Blender UI context (background mode creates the default screen).
Set BLENDER_SCULPT_PHYSICAL_BALLOON before launching Blender; Python's
os.environ updates do not reliably reach C++ getenv on Windows.
PHYSICAL_PROBE_SUBDIVISIONS (6-9 tested) and PHYSICAL_PROBE_RELIEF (0.005
tested) control the mesh fixture.

This is a diagnostic, not a golden-image test. It reports displacement
coherence and fails only on invalid/no-op geometry.
"""

import math
import os

import bpy
from mathutils import Vector


def view3d_override():
    override = bpy.context.copy()
    for area in override["screen"].areas:
        if area.type != 'VIEW_3D':
            continue
        for region in area.regions:
            if region.type == 'WINDOW':
                override["area"] = area
                override["region"] = region
                return override
    raise RuntimeError("VIEW_3D window region not found")


def positions(mesh):
    values = [0.0] * (mesh.vertices.__len__() * 3)
    mesh.vertices.foreach_get("co", values)
    return [Vector(values[i:i + 3]) for i in range(0, len(values), 3)]


def main():
    # Match Blender's sculpt_brushes_test fixture setup.
    bpy.ops.wm.read_factory_settings(use_empty=True)
    print("PROBE_STAGE cleared", flush=True)
    subdivisions = int(os.environ.get("PHYSICAL_PROBE_SUBDIVISIONS", "6"))
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=subdivisions, radius=1.0)
    obj = bpy.context.object
    mesh = obj.data
    relief = float(os.environ.get("PHYSICAL_PROBE_RELIEF", "0"))
    if relief:
        for vertex in mesh.vertices:
            position = vertex.co.copy()
            wave = math.sin(91.0 * position.x) * math.sin(87.0 * position.y) * math.sin(93.0 * position.z)
            vertex.co = position * (1.0 + relief * wave)
        mesh.update()

    override = view3d_override()
    print(f"PROBE_STAGE mesh verts={len(mesh.vertices)}", flush=True)
    with bpy.context.temp_override(**override):
        bpy.ops.view3d.view_axis(type='FRONT')
        bpy.ops.view3d.view_selected()

    bpy.ops.ed.undo_push()
    bpy.ops.sculpt.sculptmode_toggle()
    print("PROBE_STAGE sculpt_mode", flush=True)
    result = bpy.ops.brush.asset_activate(
        asset_library_type='ESSENTIALS',
        relative_asset_identifier=(
            'brushes/essentials_brushes-mesh_sculpt.blend/Brush/Inflate/Deflate'))
    if result != {'FINISHED'}:
        raise RuntimeError(f"Inflate brush activation failed: {result}")
    print("PROBE_STAGE brush_active", flush=True)

    brush = bpy.context.tool_settings.sculpt.brush
    brush.size = 180
    brush.strength = 0.18
    before = positions(mesh)
    print("PROBE_STAGE before_captured", flush=True)
    stroke = []
    for i in range(50):
        fraction = i / 49.0
        event = Vector((override["area"].width * fraction,
                        override["area"].height * fraction))
        stroke.append({
            "name": "stroke",
            "mouse": (0.0, 0.0),
            "mouse_event": event,
            "is_start": True,
            "location": (0.0, 0.0, 0.0),
            "pressure": 1.0,
            "time": 1.0,
            "size": 1.0,
            "x_tilt": 0.0,
            "y_tilt": 0.0,
        })
    with bpy.context.temp_override(**override):
        print("PROBE_STAGE stroke_begin", flush=True)
        result = bpy.ops.sculpt.brush_stroke(
            stroke=stroke, override_location=True)
    print(f"PROBE_STAGE stroke_end result={result}", flush=True)
    if result != {'FINISHED'}:
        raise RuntimeError(f"Inflate stroke failed: {result}")

    after = positions(mesh)
    displacement = [b - a for a, b in zip(before, after)]
    lengths = [d.length for d in displacement]
    changed = [i for i, length in enumerate(lengths) if length > 1.0e-8]
    if not changed:
        raise RuntimeError("Physical Inflate produced no changed vertices")
    if not all(math.isfinite(component) for d in displacement for component in d):
        raise RuntimeError("Physical Inflate produced non-finite coordinates")

    adjacency = [[] for _ in displacement]
    for edge in mesh.edges:
        a, b = edge.vertices
        adjacency[a].append(b)
        adjacency[b].append(a)
    residual_sq = 0.0
    displacement_sq = 0.0
    measured = 0
    for vertex in changed:
        neighbors = [n for n in adjacency[vertex] if lengths[n] > 1.0e-8]
        if not neighbors:
            continue
        average = sum((displacement[n] for n in neighbors), Vector()) / len(neighbors)
        residual_sq += (displacement[vertex] - average).length_squared
        displacement_sq += displacement[vertex].length_squared
        measured += 1
    residual_ratio = math.sqrt(residual_sq / max(displacement_sq, 1.0e-30))
    rms = math.sqrt(sum(lengths[i] ** 2 for i in changed) / len(changed))
    peak = max(lengths)
    print(
        "INFLATE_DENSE_PROBE_OK "
        f"verts={len(mesh.vertices)} changed={len(changed)} measured={measured} "
        f"rms={rms:.9f} peak={peak:.9f} peak_over_rms={peak / rms:.6f} "
        f"neighbor_residual_ratio={residual_ratio:.6f}")


if __name__ == "__main__":
    main()
