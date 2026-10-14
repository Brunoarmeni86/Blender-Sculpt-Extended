"""Factory GUI scene only: native selection Undo, bake, deeper history and save.
No save or mode switches are inserted between the stroke and its first Undo.
"""
import bpy
import json
import os
from pathlib import Path
import sys
import time
import traceback
from mathutils import Quaternion, Vector
from bpy_extras.view3d_utils import location_3d_to_region_2d

out = Path(sys.argv[sys.argv.index('--') + 1])
level = int(sys.argv[sys.argv.index('--') + 2])
out.mkdir(parents=True, exist_ok=True)

name = 'Conform-Undo-Cache-Fixture'
report = {'status':'RUNNING','level':level,'total':5,'geometry':{},'states':{},'audit_labels':[],
          'flags':{k:os.getenv(k) for k in ('BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD',)},
          'scope':'Factory scene only; Conform and first stroke global Undo, no user files'}

def required(result, label):
    if 'FINISHED' not in result:
        raise RuntimeError(label + ': ' + str(result))

def obj():
    result = bpy.data.objects.get(name)
    if result is None:
        raise RuntimeError('Fixture missing after Undo')
    bpy.context.view_layer.objects.active = result
    result.select_set(True)
    return result

def state():
    ob = obj()
    m = ob.modifiers['Multires']
    return {'mode':ob.mode,'sculpt':m.sculpt_levels,'total':m.total_levels,
            'active':m.active_sculpt_layer_index,'recording':m.sculpt_layer_recording,
            'layers':[{'name':l.name,'strength':l.strength,'visible':l.visible} for l in m.sculpt_layers]}

def audit(label):
    required(bpy.ops.sculpt.multires_base_fingerprint(label=label), label)
    report['audit_labels'].append(label)
    report['states'][label] = state()
    if label in ('baseline','before-undo','after-undo','after-redo','after-repeat-undo','after-repeat-redo','after-commit'):
        if report['states'][label]['mode'] != 'SCULPT':
            raise RuntimeError('Native stroke history changed mode at '+label+': '+report['states'][label]['mode'])

def viewport():
    window = bpy.context.window or next(iter(bpy.context.window_manager.windows), None)
    if not window:
        raise RuntimeError('Native regression test requires a window')
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    return window, area, region, area.spaces.active.region_3d

def setup():
    bpy.context.preferences.edit.use_global_undo = True
    bpy.context.preferences.edit.undo_steps = 64
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    required(bpy.ops.mesh.primitive_grid_add(x_subdivisions=5,y_subdivisions=5,size=2), 'Grid')
    ob = bpy.context.object
    ob.name = name
    m = ob.modifiers.new('Multires','MULTIRES')
    for _ in range(5):
        required(bpy.ops.object.multires_subdivide(modifier=m.name), 'Subdivision')
    m.levels = m.sculpt_levels = m.render_levels = 5
    required(bpy.ops.object.mode_set(mode='SCULPT'), 'Sculpt')
    for i, layer_name in enumerate(('Hidden Detail', 'Main Form')):
        required(bpy.ops.sculpt.multires_layer_add(), 'Add layer')
        m.sculpt_layers[-1].name = layer_name
        texture = bpy.data.textures.new('Fixture Gradient ' + str(i), type='BLEND')
        m.sculpt_displacement_texture = texture
        m.sculpt_displacement_coords = 'LOCAL'
        m.sculpt_displacement_strength = .025 + i * .035
        m.sculpt_displacement_midlevel = 0.
        m.use_sculpt_displacement = True
        required(bpy.ops.sculpt.multires_texture_displacement_bake(respect_mask=False,disable_live=True), 'Bake fixture texture')
        m.sculpt_layers[-1].visible = False
    required(bpy.ops.sculpt.multires_layer_add(), 'Add sparse layer')
    m.sculpt_layers[-1].name = 'Hidden Empty'
    m.sculpt_layers[-1].visible = False
    m.sculpt_layers[1].visible = True
    m.sculpt_layers[1].strength = .6
    m.sculpt_levels = level
    m.levels = level
    bpy.context.view_layer.update()
    brush = None
    for brush_name in ('Clay Strips', 'Draw', 'Clay'):
        try:
            required(bpy.ops.brush.asset_activate(asset_library_type='ESSENTIALS',relative_asset_identifier=f'brushes/essentials_brushes-mesh_sculpt.blend/Brush/{brush_name}'), 'Brush asset')
            candidate = bpy.context.scene.tool_settings.sculpt.brush
            if candidate and candidate.sculpt_brush_type != 'PAINT':
                brush = candidate
                break
        except Exception as error:
            print('BRUSH_ASSET_ATTEMPT', brush_name, error, flush=True)
    if not brush:
        raise RuntimeError('No compatible Sculpt brush asset')
    brush.size = 120
    brush.strength = .5
    for prop in ('use_pressure_size','use_pressure_strength'):
        if hasattr(brush, prop):
            setattr(brush, prop, False)
    unified = bpy.context.scene.tool_settings.sculpt.unified_paint_settings
    for prop in ('use_unified_size','use_unified_strength'):
        if hasattr(unified, prop):
            setattr(unified, prop, False)
    _, area, _, rv3d = viewport()
    rv3d.view_rotation = Quaternion((1,0,0,0))
    rv3d.view_location = Vector((0,0,0))
    rv3d.view_distance = 4.
    rv3d.view_perspective = 'ORTHO'
    area.tag_redraw()
    # Real operator checkpoint preceding the first stroke; active layer is
    # deliberately non-empty, not last, and Sculpt 2 differs from maximum 5.
    required(bpy.ops.sculpt.multires_layer_set_active(index=0), 'Select previous hidden layer')
    bpy.context.view_layer.update()

    required(bpy.ops.sculpt.multires_layer_set_active(index=1), 'Select Main Form')
    m.sculpt_layer_recording = True
    bpy.context.view_layer.update()
    required(bpy.ops.object.mode_set(mode='OBJECT'), 'Object before Conform')
    required(bpy.ops.ed.undo_push(message='Before Conform Cache Test'),'Pre-Conform checkpoint')

def stroke():
    ob = obj()
    if ob.mode != 'SCULPT':
        raise RuntimeError('Unexpected mode before stroke')
    window, area, region, rv3d = viewport()
    projected = location_3d_to_region_2d(region, rv3d, Vector((0.,0.,.04)))
    if projected is None or not (0 < projected.x < region.width and 0 < projected.y < region.height):
        raise RuntimeError('Stroke point outside viewport')
    props = bpy.types.OperatorStrokeElement.bl_rna.properties
    elements = []
    for i in range(3):
        mouse = (projected.x + i*3, projected.y + i*2)
        data = {'name':'native-test','mouse':mouse,'mouse_event':mouse,'location':(0.,0.,.04),
                'pressure':1.,'size':120.,'time':i*.12,'is_start':i==0,'pen_flip':False,'x_tilt':0.,'y_tilt':0.}
        elements.append({k:v for k,v in data.items() if props.get(k) is not None})
    with bpy.context.temp_override(window=window,area=area,region=region):
        required(bpy.ops.sculpt.brush_stroke('EXEC_DEFAULT',True,stroke=elements,mode='NORMAL'), 'Sculpt stroke')

def commit_stroke():
    window, area, region, _ = viewport()
    with bpy.context.temp_override(window=window,area=area,region=region):
        ob = obj()
        report['commit_context'] = {'fixture_mode':ob.mode,'context_mode':bpy.context.mode,
            'active':bpy.context.active_object.name if bpy.context.active_object else None}
        if ob.mode != 'SCULPT':
            raise RuntimeError('Commit precondition failed: Undo/Redo left the fixture in '+ob.mode)
        required(bpy.ops.sculpt.multires_layer_commit('EXEC_DEFAULT',True),'Commit restored stroke')
        bpy.context.view_layer.update()
        audit('after-commit')

def capture_geometry(label):
    ob = obj()
    if ob.mode != 'OBJECT':
        raise RuntimeError('Object Mode required for geometry comparison')
    m = ob.modifiers['Multires']
    old = m.levels
    points = {}
    try:
        for level in range(m.total_levels+1):
            m.levels = level
            bpy.context.view_layer.update()
            evaluated = ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            try:
                points[str(level)] = [list(v.co) for v in mesh.vertices]
            finally:
                evaluated.to_mesh_clear()
    finally:
        m.levels = old
        bpy.context.view_layer.update()
    report['geometry'][label] = points

def conform():
    # Its Object Mode wrapper already calls ED_undo_push; do not add a second step.
    required(bpy.ops.object.multires_conform_layers('EXEC_DEFAULT',False,modifier='Multires',include_visible_layers=True),'Conform Base layer-aware')
    bpy.context.view_layer.update()
    audit('post-conform')
    capture_geometry('post-conform')

def sculpt_again():
    required(bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='SCULPT'),'Sculpt after Conform')
    required(bpy.ops.sculpt.multires_layer_set_active('EXEC_DEFAULT',True,index=0),'Select hidden layer')
    required(bpy.ops.sculpt.multires_layer_set_active('EXEC_DEFAULT',True,index=1),'Select main layer')
    obj().modifiers['Multires'].sculpt_layer_recording=True
    bpy.context.view_layer.update()

steps=[
    lambda:(audit('pre-conform'),capture_geometry('pre-conform')),
    conform,
    lambda:(required(bpy.ops.ed.undo(),'Conform Undo'),bpy.context.view_layer.update(),audit('conform-undo'),capture_geometry('conform-undo')),
    lambda:(required(bpy.ops.ed.redo(),'Conform Redo'),bpy.context.view_layer.update(),audit('conform-redo'),capture_geometry('conform-redo')),
    sculpt_again,
    lambda:audit('baseline'),
    lambda:(stroke(),audit('before-undo')),
    lambda:(required(bpy.ops.ed.undo(),'First stroke Undo'),audit('after-undo')),
    lambda:(required(bpy.ops.ed.redo(),'First stroke Redo'),audit('after-redo')),
    lambda:(required(bpy.ops.ed.undo(),'Repeated stroke Undo'),audit('after-repeat-undo')),
    lambda:(required(bpy.ops.ed.redo(),'Repeated stroke Redo'),audit('after-repeat-redo')),
    commit_stroke,
    lambda:required(bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='OBJECT'),'Object before save'),
    lambda:(audit('before-save'),capture_geometry('before-save')),
    lambda:required(bpy.ops.wm.save_as_mainfile(filepath=str(out/'Conform-Native.blend'),copy=True,check_existing=False),'Save fixture'),
]
index=-1
def tick():
    global index
    try:
        if index==-1:
            setup();index=0;return .8
        if index<len(steps):
            steps[index]();index+=1;return .8
        report['status']='PASS'
        (out/'ui-test.json').write_text(json.dumps(report),encoding='utf-8')
        print('CONFORM_UNDO_CACHE_SEQUENCE PASS',level,flush=True)
        bpy.ops.wm.quit_blender();return None
    except Exception:
        report['status']='FAIL';report['error']=traceback.format_exc()
        (out/'ui-test.json').write_text(json.dumps(report),encoding='utf-8')
        traceback.print_exc();bpy.ops.wm.quit_blender();return None
bpy.app.timers.register(tick,first_interval=1.)
