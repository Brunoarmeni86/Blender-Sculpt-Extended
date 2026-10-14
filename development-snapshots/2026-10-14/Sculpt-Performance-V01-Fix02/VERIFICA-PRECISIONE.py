"""Factory-scene test run by the Windows kit before publishing its ZIP."""
import bpy
import json
import math
from pathlib import Path
import sys

directory = Path(sys.argv[sys.argv.index('--')+1])
directory.mkdir(parents=True, exist_ok=True)
report = {'status':'RUNNING', 'paint_undo_tested':False, 'high_color_master_tested':False,
          'delete_undo_redo_tested':False, 'cases':[]}

def required(result, label):
    if 'FINISHED' not in result:
        raise RuntimeError(label+': '+str(result))

def points(obj):
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        result = [tuple(v.co) for v in mesh.vertices]
        if not result or not all(math.isfinite(x) for p in result for x in p):
            raise RuntimeError('Invalid evaluated mesh')
        return result
    finally:
        evaluated.to_mesh_clear()

def difference(a, b):
    if len(a) != len(b):
        raise RuntimeError('Retained-level topology changed')
    return max(abs(x-y) for p,q in zip(a,b) for x,y in zip(p,q))

def subdivision_comparison(before, after, control_before, control_after):
    initial_difference = difference(before,control_before)
    if len(before) != len(after) or len(control_before) != len(control_after):
        raise RuntimeError('Subdivision changed retained topology')
    movement = [tuple(y-x for x,y in zip(p,q)) for p,q in zip(before,after)]
    control_movement = [tuple(y-x for x,y in zip(p,q)) for p,q in zip(control_before,control_after)]
    movement_difference = difference(movement,control_movement)
    if initial_difference > 5e-4 or movement_difference > 1e-5:
        raise RuntimeError(f'Subdivision regression versus existing layer control: initial={initial_difference:.9g}, movement={movement_difference:.9g}')
    return initial_difference,movement_difference

def metadata(mmd):
    return [(layer.name,layer.strength,layer.visible) for layer in mmd.sculpt_layers]

def cancelled(obj, mmd, label):
    total, original = mmd.total_levels, metadata(mmd)
    try:
        result = bpy.ops.object.multires_higher_levels_delete(modifier=mmd.name)
    except RuntimeError:
        result = {'CANCELLED'}
    if 'CANCELLED' not in result or mmd.total_levels != total or metadata(mmd) != original:
        raise RuntimeError(label+': deletion was not rejected without changing levels/layers')

def fixture(name, levels=3):
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        required(bpy.ops.object.mode_set(mode='OBJECT'), 'Leave mode')
    for obj in list(bpy.data.objects):
        if obj.type == 'MESH':
            bpy.data.objects.remove(obj, do_unlink=True)
    required(bpy.ops.mesh.primitive_cube_add(), 'Cube')
    obj = bpy.context.object
    obj.name = name
    mmd = obj.modifiers.new('Multires', 'MULTIRES')
    for _ in range(levels):
        required(bpy.ops.object.multires_subdivide(modifier=mmd.name), 'Subdivide fixture')
    mmd.levels = mmd.sculpt_levels = mmd.render_levels = levels
    required(bpy.ops.object.mode_set(mode='SCULPT'), 'Sculpt')
    required(bpy.ops.sculpt.multires_layer_add(), 'Nonzero layer')
    layer = mmd.sculpt_layers[mmd.active_sculpt_layer_index]
    layer.name = 'Retained Detail'
    texture = bpy.data.textures.new(name+' Gradient', type='BLEND')
    mmd.sculpt_displacement_texture = texture
    mmd.sculpt_displacement_coords = 'LOCAL'
    mmd.sculpt_displacement_strength = .2
    mmd.sculpt_displacement_midlevel = 0.0
    mmd.use_sculpt_displacement = True
    required(bpy.ops.sculpt.multires_texture_displacement_bake(respect_mask=False, disable_live=True), 'Bake nonzero layer')
    required(bpy.ops.sculpt.multires_layer_add(), 'Sparse empty layer')
    empty = mmd.sculpt_layers[mmd.active_sculpt_layer_index]
    empty.name = 'Empty Hidden Layer'
    empty.visible = False
    mmd.active_sculpt_layer_index = 0
    required(bpy.ops.object.mode_set(mode='OBJECT'), 'Object')
    mmd.levels = 2
    layer = mmd.sculpt_layers['Retained Detail']
    layer.strength = 0
    zero = points(obj)
    layer.strength = .65
    full = points(obj)
    if difference(zero,full) < 1e-5:
        raise RuntimeError('Fixture has no nonzero layer displacement')
    return obj, mmd

try:
    subdivision_cases = []
    for mode in ('OBJECT','SCULPT'):
        name = 'Delete-Higher-'+mode
        obj, mmd = fixture(name)
        expected = points(obj)
        original = metadata(mmd)
        linked = obj.copy()
        linked.name = name+' Shared Mesh Guard'
        bpy.context.collection.objects.link(linked)
        cancelled(obj,mmd,'Shared mesh guard')
        bpy.data.objects.remove(linked,do_unlink=True)
        mmd.sculpt_layer_recording = True
        rec = mmd.sculpt_layer_recording
        if not rec:
            raise RuntimeError('Cannot initialize REC fixture')
        if mode == 'SCULPT':
            mmd.levels = 3
            mmd.sculpt_levels = 2
            required(bpy.ops.object.mode_set(mode='SCULPT'), 'Enter Sculpt for deletion')
        required(bpy.ops.object.multires_higher_levels_delete(modifier=mmd.name), 'Delete Higher with layers')
        if mmd.total_levels != 2 or mmd.levels > 2 or mmd.sculpt_levels > 2 or mmd.render_levels > 2:
            raise RuntimeError('Deleted level is still referenced')
        if metadata(mmd) != original or mmd.sculpt_layer_recording != rec:
            raise RuntimeError('Deletion changed layer identity, weight, visibility or REC')
        if mode == 'SCULPT':
            required(bpy.ops.object.mode_set(mode='OBJECT'), 'Return Object after deletion')
        retained_delta = difference(expected,points(obj))
        if retained_delta > 5e-4:
            raise RuntimeError('Deleting higher levels changed retained-level geometry')
        cancelled(obj,mmd,'No-higher-level guard')
        path = directory/(name+'.blend')
        required(bpy.ops.wm.save_as_mainfile(filepath=str(path),check_existing=False), 'Save truncated layers')
        required(bpy.ops.wm.open_mainfile(filepath=str(path)), 'Reload truncated layers')
        obj = bpy.data.objects[name]
        mmd = next(mod for mod in obj.modifiers if mod.type == 'MULTIRES')
        if mmd.total_levels != 2 or metadata(mmd) != original or difference(expected,points(obj)) > 5e-4:
            raise RuntimeError('Reload changed truncated layers')
        required(bpy.ops.object.multires_subdivide(modifier=mmd.name), 'Subdivide truncated layers again')
        if mmd.total_levels != 3:
            raise RuntimeError('Cannot subdivide after truncation')
        mmd.levels = 2
        after_subdivision = points(obj)
        subdiv_delta = difference(expected,after_subdivision)
        print(f'DELETE_HIGHER_CASE mode={mode} delete_delta={retained_delta:.9g} subdiv_delta={subdiv_delta:.9g}',flush=True)
        subdivision_cases.append((mode,expected,after_subdivision))
        report['cases'].append({'mode':mode,'delete_higher':True,'retained_geometry':True,
                                'layer_metadata':True,'rec':True,'shared_mesh_guard':True,
                                'save_reload':True,'subdivide_operator_finished':True,
                                'subdivide_retained_geometry':subdiv_delta <= 5e-4,
                                'delete_position_delta':retained_delta,'subdivide_position_delta':subdiv_delta,'scene':str(path)})
    # Differential control: the same layer fixture starts directly at level 2.
    # It has never run Delete Higher. The direct deletion/reload geometry gate
    # remains strict; subdivision must match the entire existing movement field.
    obj,mmd = fixture('Subdivision-Without-Delete-Control',levels=2)
    control_before = points(obj)
    required(bpy.ops.object.multires_subdivide(modifier=mmd.name),'Control subdivide without Delete Higher')
    mmd.levels = 2
    control_after = points(obj)
    control_delta = difference(control_before,control_after)
    report['subdivision_without_delete'] = {'position_delta':control_delta,'retained_geometry':control_delta <= 5e-4}
    print(f'SUBDIVISION_WITHOUT_DELETE layer_position_delta={control_delta:.9g}',flush=True)
    for mode,before,after in subdivision_cases:
        initial_error,movement_error = subdivision_comparison(before,after,control_before,control_after)
        row = next(case for case in report['cases'] if case['mode']==mode)
        row.update(subdivision_matches_existing_control=True,
                   control_initial_position_error=initial_error,control_movement_error=movement_error)
        print(f'SUBDIVISION_DIFFERENTIAL_PASS mode={mode} initial_error={initial_error:.9g} movement_error={movement_error:.9g}',flush=True)
    # A standard Multires control has no custom layer data at all.
    required(bpy.ops.mesh.primitive_cube_add(),'Standard control cube')
    obj = bpy.context.object
    mmd = obj.modifiers.new('Standard Multires Control','MULTIRES')
    for _ in range(2):
        required(bpy.ops.object.multires_subdivide(modifier=mmd.name),'Standard control subdivision')
    mmd.levels = 2
    expected = points(obj)
    required(bpy.ops.object.multires_subdivide(modifier=mmd.name),'Standard control new level')
    mmd.levels = 2
    standard_delta = difference(expected,points(obj))
    report['standard_multires_subdivision'] = {'position_delta':standard_delta,'retained_geometry':standard_delta <= 5e-4}
    print(f'STANDARD_MULTIRES_SUBDIVISION position_delta={standard_delta:.9g}',flush=True)
    if standard_delta > 5e-4:
        raise RuntimeError('Standard Multires subdivision control changed the retained surface')
    report['known_issues'] = []
    if control_delta > 5e-4:
        report['known_issues'].append('Existing Sculpt Layer subdivision changes retained geometry; reproduced without Delete Higher')
        print('KNOWN_ISSUE: Sculpt Layer subdivision changes retained geometry equally without Delete Higher; this build does not fix that separate path',flush=True)
    if control_delta > 1e-5 or any(c['subdivide_position_delta'] > 1e-5 for c in report['cases']):
        raise RuntimeError('Retained subdivision still moves beyond 1e-5')
    report['status'] = 'PASS'
    print('DELETE_HIGHER_NATIVE PASS: Object/Sculpt, retained surface, layers, REC, guards, reload and subdivision differential control',flush=True)
except Exception as exc:
    report.update(status='FAIL',error=str(exc))
    raise
finally:
    (directory/'delete-higher-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
