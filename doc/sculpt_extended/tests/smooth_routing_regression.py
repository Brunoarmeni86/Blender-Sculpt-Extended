import bpy
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import traceback
import numpy as np
from bpy_extras import view3d_utils

root = Path(__file__).parent / 'SMOOTH-ROUTING-20261025'
phase = os.environ.get('SMOOTH_ROUTING_PHASE', 'before')
root.mkdir(exist_ok=True)
results = []
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.edit.use_global_undo = True

class LayerView(ctypes.Structure):
    _fields_ = [('next',ctypes.c_void_p),('prev',ctypes.c_void_p),('name',ctypes.c_char*64),
                ('strength',ctypes.c_float),('flag',ctypes.c_int),('level',ctypes.c_int),
                ('grids_num',ctypes.c_int),('disps',ctypes.c_void_p),('base_deltas',ctypes.c_void_p),
                ('base_verts_num',ctypes.c_int)]

def base_data():
    layer = modifier().sculpt_layers[0]
    raw = LayerView.from_address(layer.as_pointer())
    assert raw.name.decode() == layer.name and raw.strength == layer.strength
    assert raw.base_deltas and raw.base_verts_num == len(bpy.context.object.data.vertices)
    values = (ctypes.c_float * (raw.base_verts_num * 3)).from_address(raw.base_deltas)
    return np.array(values, dtype=np.float32)

def modifier():
    return bpy.context.object.modifiers['Multires']

def context():
    window = bpy.context.window
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    return bpy.context.temp_override(window=window, area=area,
                                    region=next(r for r in area.regions if r.type == 'WINDOW'))

def positions():
    path = root / 'probe.json'
    with context():
        assert bpy.ops.sculpt.polypaint_blur_probe_v01(action='EXPORT',filepath=str(path)) == {'FINISHED'}
    return np.asarray(json.loads(path.read_text())['positions'], dtype=np.float32)

def brush(name, preserve=False):
    with context():
        assert bpy.ops.brush.asset_activate(asset_library_type='ESSENTIALS',
            relative_asset_identifier='brushes/essentials_brushes-mesh_sculpt.blend/Brush/' + name) == {'FINISHED'}
        b = bpy.context.tool_settings.sculpt.brush
        b.sculpt_preserve_other_layers = preserve
        b.use_locked_size = 'SCENE'
        b.unprojected_size = 2.5
        b.size = 128
        b.strength = .6
        b.use_pressure_size = b.use_pressure_strength = False
        b.stroke_method = 'SPACE'
        b.spacing = 10
        b.jitter = 0
        b.use_smooth_stroke = False
        b.auto_smooth_factor = 0
        unified = getattr(bpy.context.tool_settings.sculpt, 'unified_paint_settings', None)
        if unified:
            unified.use_unified_size = unified.use_unified_strength = False
        for owner in [b, bpy.context.tool_settings.sculpt]:
            for prop in owner.bl_rna.properties:
                if prop.identifier.startswith('use_automasking_') and prop.type == 'BOOLEAN' and not prop.is_readonly:
                    setattr(owner, prop.identifier, False)
        return b

def stroke(toggle='None',commit=True):
    with context():
        region, rv = bpy.context.region, bpy.context.space_data.region_3d
        samples = []
        for i, offset in enumerate([-30,-15,0,15,30]):
            mouse = (region.width/2 + offset, region.height/2)
            origin = view3d_utils.region_2d_to_origin_3d(region, rv, mouse)
            direction = view3d_utils.region_2d_to_vector_3d(region, rv, mouse)
            hit, location, *_ = bpy.context.scene.ray_cast(bpy.context.evaluated_depsgraph_get(), origin, direction)
            assert hit, 'Stroke missed fixture'
            samples.append(dict(name='SmoothRouting',location=tuple(location),mouse=mouse,mouse_event=mouse,
                pressure=1.,size=128.,x_tilt=0.,y_tilt=0.,time=i*.1,is_start=i==0))
        assert bpy.ops.sculpt.brush_stroke('EXEC_DEFAULT',True,stroke=samples,
                    override_location=True,brush_toggle=toggle) == {'FINISHED'}
        if commit:
            assert bpy.ops.sculpt.multires_layer_commit() == {'FINISHED'}
    bpy.context.view_layer.update()

def save_result():
    (root / (phase + '.json')).write_text(json.dumps(results,indent=2))

def run():
    with context():
        if bpy.context.object and bpy.context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj = bpy.context.object
        obj.name = 'L0_Smooth_Routing_Test'
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.subdivide(number_cuts=7)
        bpy.ops.object.mode_set(mode='OBJECT')
        for v in obj.data.vertices:
            q = v.co.normalized()
            v.co = q * (1 + .06 * math.sin(q.x*15) * math.cos(q.y*13))
        obj.data.use_mirror_x = obj.data.use_mirror_y = obj.data.use_mirror_z = False
        obj.modifiers.new('Multires', 'MULTIRES')
        bpy.ops.object.mode_set(mode='SCULPT')
        bpy.ops.view3d.view_axis(type='TOP',align_active=False)
        bpy.context.space_data.region_3d.view_distance = 4
        brush('Draw')
        assert bpy.ops.sculpt.multires_layer_add() == {'FINISHED'}
        modifier().sculpt_layers[0].name = 'Real L0 edits'
    yield .6
    stroke()
    initial = base_data()
    assert np.max(np.abs(initial)) > 1e-5
    # Smooth itself must work on an unsplit Multires layer, then propagate.
    brush('Smooth', preserve=False)
    l0_before = base_data()
    stroke()
    l0_after = base_data()
    assert np.max(np.abs(l0_after-l0_before)) > 1e-6
    with context():
        for _ in range(int(os.environ.get('SMOOTH_TEST_LEVELS','2'))):
            assert bpy.ops.object.multires_subdivide(modifier='Multires') == {'FINISHED'}
        modifier().levels = modifier().sculpt_levels = modifier().total_levels
    yield .5
    assert np.array_equal(base_data(),l0_after), 'Subdivide changed L0 layer payload'
    shown = positions()
    with context(): modifier().sculpt_layers[0].visible = False
    yield .3
    hidden = positions()
    assert np.max(np.abs(shown-hidden)) > 1e-5, 'L0 contribution missing after Subdivide'
    with context(): modifier().sculpt_layers[0].visible = True
    yield .3
    seed = root / ('seed-' + phase + '.blend')
    with context(): bpy.ops.wm.save_as_mainfile(filepath=str(seed),check_existing=False)
    results.append(dict(case='L0_smooth_then_subdivide',status='PASS',
        l0_max=float(np.max(np.abs(l0_after))),high_level_contribution=float(np.max(np.abs(shown-hidden)))))
    save_result()
    cases = ['smooth_off','shift_off','autosmooth_off','smooth_on_guard']
    if phase == 'single_layer':
        cases += ['smooth_on_single','shift_on_single','autosmooth_on_single','smooth_on_hidden_other']
    for case in cases:
        with context(): bpy.ops.wm.open_mainfile(filepath=str(seed))
        yield .4
        preserve = '_on_' in case
        if phase == 'single_layer' and case in ['smooth_on_guard','smooth_on_hidden_other']:
            with context(): assert bpy.ops.sculpt.multires_layer_add() == {'FINISHED'}
            brush('Draw', preserve=False)
            stroke()
            with context():
                modifier().sculpt_layers[1].visible = case=='smooth_on_guard'
                modifier().active_sculpt_layer_index = 0
        if phase == 'single_layer' and preserve:
            with context(): modifier().sculpt_layers[0].strength = .4
        brush('Smooth' if case.startswith('smooth') else 'Draw', preserve=preserve)
        if case == 'shift_off':
            # The temporary Smooth asset deliberately disagrees with the original Draw choice.
            original = bpy.context.tool_settings.sculpt.brush
            smooth = brush('Smooth', preserve=True)
            brush('Draw', preserve=False)
        if case.startswith('autosmooth'): bpy.context.tool_settings.sculpt.brush.auto_smooth_factor = .5
        before, base_before = positions(), base_data()
        status, message = 'FINISHED', ''
        try:
            stroke('SMOOTH' if case.startswith('shift') else 'None')
        except RuntimeError as error:
            status, message = 'BLOCKED', str(error)
        yield .2
        after = positions()
        change = float(np.max(np.abs(after-before)))
        expected_block = phase=='before' or case=='smooth_on_guard'
        assert (status=='BLOCKED') == expected_block, (case,status,message)
        if expected_block:
            assert 'L0 cage edits' in message and change < 1e-7
        else:
            assert change > 1e-6, (case,'Brush did not change surface',change)
        assert np.array_equal(base_before,base_data()), (case,'High level stroke changed L0 payload')
        results.append(dict(case=case,status='PASS',stroke_status=status,max_change=change,message=message))
        save_result()
        if phase != 'before' and case == 'smooth_off':
            with context(): assert bpy.ops.ed.undo() == {'FINISHED'}
            yield .3
            assert np.max(np.abs(positions()-before)) < 2e-6, 'Undo did not restore the surface'
            assert np.array_equal(base_before,base_data()), 'Undo changed L0 data'
            with context(): assert bpy.ops.ed.redo() == {'FINISHED'}
            yield .3
            assert np.max(np.abs(positions()-after)) < 2e-6, 'Redo did not restore the surface'
            saved = root / 'fixed-smooth-save.blend'
            with context():
                bpy.ops.wm.save_as_mainfile(filepath=str(saved),check_existing=False)
                bpy.ops.wm.open_mainfile(filepath=str(saved))
            yield .4
            assert np.max(np.abs(positions()-after)) < 2e-6, 'Reopen changed the surface'
            assert np.array_equal(base_before,base_data()), 'Reopen changed L0 data'
            results.append(dict(case='native_smooth_undo_redo_reopen',status='PASS'))
            save_result()
    if phase != 'before':
        with context():
            bpy.ops.wm.open_mainfile(filepath=str(seed))
        yield .3
        with context(): assert bpy.ops.sculpt.multires_layer_add() == {'FINISHED'}
        brush('Draw',preserve=False)
        stroke()
        before, base_before = positions(), base_data()
        brush('Smooth',preserve=True)
        stroke()
        yield .2
        change = float(np.max(np.abs(positions()-before)))
        assert change > 1e-6, 'Delta Smooth did not modify the supported high-resolution layer'
        assert np.array_equal(base_before,base_data()), 'Delta Smooth changed the other L0 layer'
        results.append(dict(case='isolated_smooth_supported_layer',status='PASS',max_change=change))
        save_result()
    with context(): bpy.ops.wm.quit_blender()

steps = run()
def tick():
    try: return next(steps)
    except StopIteration: return None
    except Exception:
        error = traceback.format_exc()
        (root / (phase + '-ERROR.txt')).write_text(error)
        print(error,flush=True)
        os._exit(1)
bpy.app.timers.register(tick,first_interval=1,persistent=True)
