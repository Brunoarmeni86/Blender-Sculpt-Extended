"""Exercise the bounded sparse-payload index cache with two populated layers."""

import bpy
import json
import math
import os
import time
import traceback
from pathlib import Path
from bpy_extras import view3d_utils


OUT = Path(os.environ.get("SCULPT_LAYER_MULTICACHE_PROBE", "sculpt-layer-multicache-probe"))
OUT.mkdir(parents=True, exist_ok=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.edit.use_global_undo = False


def view_context():
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    return bpy.context.temp_override(window=window, area=area, region=region)


def activate_draw_brush():
    with view_context():
        result = bpy.ops.brush.asset_activate(
            asset_library_type="ESSENTIALS",
            relative_asset_identifier=(
                "brushes/essentials_brushes-mesh_sculpt.blend/Brush/Draw"))
        if result == {"CANCELLED"}:
            return False
        assert result == {"FINISHED"}, result
        brush = bpy.context.tool_settings.sculpt.brush
        brush.use_locked_size = "SCENE"
        brush.unprojected_size = 0.18
        brush.size = 64
        brush.strength = 0.25
        brush.use_pressure_size = False
        brush.use_pressure_strength = False
        brush.stroke_method = "SPACE"
        brush.spacing = 12
        brush.jitter = 0
        brush.use_smooth_stroke = False
        brush.auto_smooth_factor = 0
        return True


def stroke(x_shift):
    with view_context():
        region = bpy.context.region
        rv3d = bpy.context.space_data.region_3d
        samples = []
        for i, offset in enumerate((-12, -6, 0, 6, 12)):
            mouse = (region.width * 0.5 + x_shift + offset, region.height * 0.5 + 28)
            origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, mouse)
            direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, mouse)
            hit, location, *_ = bpy.context.scene.ray_cast(
                bpy.context.evaluated_depsgraph_get(), origin, direction)
            assert hit, ("stroke missed fixture", mouse)
            samples.append(dict(
                name="SculptLayerMulticacheProbe", location=tuple(location),
                mouse=mouse, mouse_event=mouse, pressure=1.0, size=64.0,
                x_tilt=0.0, y_tilt=0.0, time=i * 0.1, is_start=i == 0))
        result = bpy.ops.sculpt.brush_stroke(
            "EXEC_DEFAULT", True, stroke=samples, override_location=True,
            brush_toggle="None")
        assert result == {"FINISHED"}, result


def setup():
    with view_context():
        if bpy.context.object and bpy.context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.subdivide(number_cuts=5)
        bpy.ops.object.mode_set(mode="OBJECT")
        for vertex in obj.data.vertices:
            vertex.co = vertex.co.normalized()
        modifier = obj.modifiers.new("Multires", "MULTIRES")
        bpy.ops.object.mode_set(mode="SCULPT")
        bpy.ops.view3d.view_axis(type="TOP", align_active=False)
        bpy.context.space_data.region_3d.view_distance = 3.5
        assert bpy.ops.sculpt.multires_layer_add() == {"FINISHED"}
        for _ in range(2):
            assert bpy.ops.object.multires_subdivide(modifier=modifier.name) == {"FINISHED"}
        modifier.levels = modifier.sculpt_levels = modifier.total_levels
        return modifier


def run():
    modifier = setup()
    deadline = time.perf_counter() + 20.0
    while not activate_draw_brush():
        if time.perf_counter() >= deadline:
            raise RuntimeError("Draw brush asset did not load within 20 seconds")
        yield 0.25
    yield 0.5

    stroke(-42)
    with view_context():
        assert bpy.ops.sculpt.multires_layer_commit() == {"FINISHED"}
        assert bpy.ops.sculpt.multires_layer_add() == {"FINISHED"}
    stroke(42)
    with view_context():
        assert bpy.ops.sculpt.multires_layer_commit() == {"FINISHED"}
    assert len(modifier.sculpt_layers) == 2

    timings = []
    for cycle in range(8):
        for index in (0, 1):
            layer = modifier.sculpt_layers[index]
            started = time.perf_counter()
            layer.strength = 0.75 if layer.strength == 1.0 else 1.0
            bpy.context.view_layer.update()
            timings.append({
                "cycle": cycle,
                "layer": index,
                "strength": layer.strength,
                "milliseconds": (time.perf_counter() - started) * 1000.0,
            })
        yield 0.05

    summary = {"layers": 2, "switches": timings}
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2))
    (OUT / "PASS.txt").write_text("PASS\n")
    print("[SCULPT_LAYER_MULTICACHE_RESULT]", json.dumps(summary), flush=True)


steps = run()


def tick():
    try:
        return next(steps)
    except StopIteration:
        bpy.ops.wm.quit_blender()
        return None
    except Exception:
        error = traceback.format_exc()
        (OUT / "FAIL.txt").write_text(error)
        print(error, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(tick, first_interval=0.5, persistent=True)
