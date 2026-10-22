"""Regression: regular Smooth must work on Base Mesh without a visible layer."""
import bpy
import json
import sys
import traceback
import numpy as np
from pathlib import Path

output = Path(sys.argv[sys.argv.index('--') + 1])
cage = '--cage' in sys.argv
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False


def view_context():
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    return bpy.context.temp_override(window=window, area=area, region=region)


def base_positions(obj):
    values = [0.0] * (len(obj.data.vertices) * 3)
    obj.data.vertices.foreach_get('co', values)
    return values


def surface_positions():
    path = output.with_suffix('.probe.json')
    with view_context():
        assert bpy.ops.sculpt.polypaint_blur_probe_v01(action='EXPORT', filepath=str(path)) == {'FINISHED'}
    data = json.loads(path.read_text())
    path.unlink()
    return np.asarray(data['positions'], dtype=np.float32)


def run():
    from bpy_extras import view3d_utils

    with view_context():
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj = bpy.context.object
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.subdivide(number_cuts=5)
        bpy.ops.transform.vertex_random(offset=0.18, uniform=0.0, normal=0.0, seed=7)
        bpy.ops.object.mode_set(mode='OBJECT')
        modifier = obj.modifiers.new(name='Multires', type='MULTIRES')
        for _ in range(3):
            assert bpy.ops.object.multires_subdivide(modifier=modifier.name) == {'FINISHED'}
        modifier.levels = modifier.sculpt_levels = modifier.total_levels
        bpy.ops.object.mode_set(mode='SCULPT')
        assert bpy.ops.sculpt.multires_layer_add() == {'FINISHED'}
        layer = modifier.sculpt_layers[0]
        layer.visible = False
        layer.strength = 0.0
        modifier.use_sculpt_base_mesh = cage
        from bl_ui.space_view3d_sculpt_destination import geometric_target
        assert geometric_target(modifier)[0] == ('CAGE' if cage else 'BASE')
        assert not any(item.visible and abs(item.strength) > 1e-6 for item in modifier.sculpt_layers)
        assert bpy.ops.brush.asset_activate(
            asset_library_type='ESSENTIALS',
            relative_asset_identifier='brushes/essentials_brushes-mesh_sculpt.blend/Brush/Smooth') == {'FINISHED'}
        brush = bpy.context.tool_settings.sculpt.brush
        brush.smooth_deform_type = 'LAPLACIAN'
        brush.strength = 0.8
        brush.size = 120
        brush.use_pressure_strength = False
        bpy.ops.view3d.view_axis(type='TOP', align_active=False)
        bpy.context.space_data.region_3d.view_distance = 4.0
    yield 0.8

    before_cage = base_positions(obj)
    before = before_cage if cage else surface_positions()
    with view_context():
        region = bpy.context.region
        view = bpy.context.space_data.region_3d
        samples = []
        for index, offset in enumerate((-30, -15, 0, 15, 30)):
            mouse = (region.width / 2 + offset, region.height / 2)
            origin = view3d_utils.region_2d_to_origin_3d(region, view, mouse)
            direction = view3d_utils.region_2d_to_vector_3d(region, view, mouse)
            hit, location, *_ = bpy.context.scene.ray_cast(
                bpy.context.evaluated_depsgraph_get(), origin, direction)
            assert hit, 'Smooth stroke missed the Base Mesh fixture'
            samples.append(dict(name='BaseSmooth', location=tuple(location), mouse=mouse,
                mouse_event=mouse, pressure=1.0, size=float(brush.size), x_tilt=0.0,
                y_tilt=0.0, time=index * 0.1, is_start=index == 0))
        assert bpy.ops.sculpt.brush_stroke(
            'EXEC_DEFAULT', True, stroke=samples, override_location=True) == {'FINISHED'}
    after = base_positions(obj) if cage else surface_positions()
    maximum = float(np.max(np.abs(np.asarray(after) - np.asarray(before))))
    if not cage:
        assert base_positions(obj) == before_cage, 'Multires smoothing unexpectedly edited the cage'
    with view_context():
        bpy.ops.object.mode_set(mode='OBJECT')
    assert maximum > 1e-7, 'Smooth did not modify the Base Mesh'
    output.write_text(json.dumps({
        'status': 'PASS',
        'target': 'BASE',
        'sculpt_base_mesh': cage,
        'visible_nonzero_layers': 0,
        'maximum_base_delta': maximum,
    }, indent=2))
    bpy.ops.wm.quit_blender()


steps = run()


def tick():
    try:
        return next(steps)
    except StopIteration:
        return None
    except Exception:
        output.with_suffix('.error.txt').write_text(traceback.format_exc())
        print(traceback.format_exc(), flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(tick, first_interval=1.0, persistent=True)
