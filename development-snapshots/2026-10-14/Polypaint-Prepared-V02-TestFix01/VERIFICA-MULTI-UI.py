"""Factory-window test only. Saves snapshots without inserting mode switches into Undo.
Each saved copy is inspected later by a separate background Blender process.
"""
import bpy
import json
import os
from pathlib import Path
import sys
import time
import traceback
from mathutils import Quaternion,Vector
from bpy_extras.view3d_utils import location_3d_to_region_2d

sys.path.insert(0,str(Path(__file__).resolve().parent))
import validation_brush

out=Path(sys.argv[sys.argv.index('--')+1]);out.mkdir(parents=True,exist_ok=True)
report={'status':'RUNNING','snapshots':[],'strength_seconds':[], 'paint_seconds':[],
        'flags':{k:os.getenv(k) for k in ('BLENDER_SCULPT_WEIGHT_SUPPORT_COMPACT','BLENDER_SCULPT_LAYER_SPARSE_READ','BLENDER_SCULPT_POLYPAINT_PREPARED_GRID')},
        'test_scene_only':True,'undo_checked_by_saved_snapshots':True}
name='Interactive-Safety-Fixture'
variant=sys.argv[sys.argv.index('--')+2]
with_layers=variant!='lazy-no-layers'

def required(result,label):
    if 'FINISHED' not in result:raise RuntimeError(label+': '+str(result))

def viewport():
    window=bpy.context.window or next(iter(bpy.context.window_manager.windows),None)
    if not window:raise RuntimeError('Test richiede una finestra Blender; non usare --background')
    area=next((a for a in window.screen.areas if a.type=='VIEW_3D'),None)
    if not area:raise RuntimeError('VIEW_3D non disponibile')
    region=next(r for r in area.regions if r.type=='WINDOW')
    return window,area,region,area.spaces.active.region_3d

def get_object():
    obj=bpy.data.objects.get(name)
    if obj is None:raise RuntimeError('Fixture persa dopo Undo')
    bpy.context.view_layer.objects.active=obj;obj.select_set(True)
    return obj

def save(label):
    path=out/(label+'.blend')
    required(bpy.ops.wm.save_as_mainfile(filepath=str(path),copy=True,check_existing=False),'Snapshot '+label)
    obj=get_object();mmd=obj.modifiers['Multires']
    levels={a.name:a.level for a in mmd.polypaint_attributes}
    if mmd.polypaint_attribute:levels[mmd.polypaint_attribute]=mmd.polypaint_level
    active=obj.data.color_attributes.active_color
    report['snapshots'].append({'label':label,'file':path.name,'levels':levels,
                                'active':active.name if active else ''})
    print('INTERACTIVE_SNAPSHOT '+label,flush=True)

def setup():
    bpy.context.preferences.edit.use_global_undo=True
    for obj in list(bpy.data.objects):bpy.data.objects.remove(obj,do_unlink=True)
    required(bpy.ops.mesh.primitive_grid_add(x_subdivisions=5,y_subdivisions=5,size=2),'Grid')
    obj=bpy.context.object;obj.name=name;mesh=obj.data
    colour=mesh.color_attributes.new(name='SkinColor',type='FLOAT_COLOR',domain='CORNER')
    for i,x in enumerate(colour.data):x.color=(.13+.002*(i%4),.31,.19,.85)
    mesh.color_attributes.active_color_index=list(mesh.color_attributes).index(colour)
    mesh.color_attributes.render_color_index=list(mesh.color_attributes).index(colour)
    mmd=obj.modifiers.new('Multires','MULTIRES')
    for _ in range(3):required(bpy.ops.object.multires_subdivide(modifier=mmd.name),'Subdivision')
    mmd.levels=mmd.sculpt_levels=mmd.render_levels=3
    required(bpy.ops.object.mode_set(mode='SCULPT'),'Sculpt')
    if with_layers:
        required(bpy.ops.sculpt.multires_layer_add(),'Nonzero layer')
        mmd.sculpt_layers[0].name='Strength Fixture'
        texture=bpy.data.textures.new('Interactive Gradient',type='BLEND')
        mmd.sculpt_displacement_texture=texture;mmd.sculpt_displacement_coords='LOCAL'
        mmd.sculpt_displacement_strength=.08;mmd.sculpt_displacement_midlevel=0.;mmd.use_sculpt_displacement=True
        required(bpy.ops.sculpt.multires_texture_displacement_bake(respect_mask=False,disable_live=True),'Bake fixture')
        for i in range(3):required(bpy.ops.sculpt.multires_layer_add(),'Sparse empty layer')
        mmd.active_sculpt_layer_index=0
    mmd.sculpt_levels=2
    bpy.context.view_layer.update()
    brush=validation_brush.create()
    brush.color=(.85,.04,.015);brush.secondary_color=(.015,.15,.85);brush.strength=.9;brush.size=140
    for prop in ('use_pressure_strength','use_pressure_size'):
        if hasattr(brush,prop):setattr(brush,prop,False)
    unified=bpy.context.scene.tool_settings.sculpt.unified_paint_settings
    for prop in ('use_unified_color','use_unified_strength','use_unified_size'):
        if hasattr(unified,prop):setattr(unified,prop,False)
    _,area,region,rv3d=viewport();rv3d.view_rotation=Quaternion((1,0,0,0));rv3d.view_location=Vector((0,0,0));rv3d.view_distance=4.;rv3d.view_perspective='ORTHO'
    area.spaces.active.shading.color_type='VERTEX';area.tag_redraw()
    report['viewport_size']=[region.width,region.height]
    # Initial checkpoint follows asynchronous local-brush activation in tick().

def stroke(point):
    obj=get_object()
    if obj.mode!='SCULPT':raise RuntimeError('Undo non ha mantenuto Sculpt Mode')
    window,area,region,rv3d=viewport()
    projected=location_3d_to_region_2d(region,rv3d,Vector(point))
    if projected is None or not (0<projected.x<region.width and 0<projected.y<region.height):raise RuntimeError('Punto della pennellata fuori viewport')
    elements=[]
    props=bpy.types.OperatorStrokeElement.bl_rna.properties
    for i in range(3):
        px=projected.x+i*3.;py=projected.y+i*2.
        values={'name':'test','mouse':(px,py),'mouse_event':(px,py),'location':point,
                'pressure':1.,'size':140.,'time':i*.12,'is_start':i==0,'pen_flip':False,'x_tilt':0.,'y_tilt':0.}
        elements.append({k:v for k,v in values.items() if props.get(k) is not None})
    started=time.perf_counter()
    with bpy.context.temp_override(window=window,area=area,region=region):
        required(bpy.ops.sculpt.brush_stroke('EXEC_DEFAULT',True,stroke=elements,mode='NORMAL'),'Paint stroke')
    report['paint_seconds'].append(time.perf_counter()-started)

def weight(value,label):
    obj=get_object();started=time.perf_counter();obj.modifiers['Multires'].sculpt_layers[0].strength=value
    bpy.context.view_layer.update();report['strength_seconds'].append({'value':value,'seconds':time.perf_counter()-started})
    save(label)

def change_level(level):
    obj=get_object();obj.modifiers['Multires'].sculpt_levels=level;bpy.context.view_layer.update()
    required(bpy.ops.ed.undo_push(message='Paint level '+str(level)+' checkpoint'),'Level checkpoint')

def delete_and_subdivide():
    print('[LAZY_TEST] geometric-subdivide START',flush=True)
    obj=get_object();required(bpy.ops.object.mode_set(mode='OBJECT'),'Object')
    mmd=obj.modifiers['Multires'];mmd.levels=1
    required(bpy.ops.object.multires_higher_levels_delete(modifier=mmd.name),'Delete Higher')
    if mmd.total_levels!=1:raise RuntimeError('Delete Higher non ha mantenuto il livello 1')
    while mmd.total_levels<4:
        old=mmd.total_levels
        required(bpy.ops.object.multires_subdivide(modifier=mmd.name),'Subdivide')
        if mmd.total_levels!=old+1:raise RuntimeError('Subdivision non completata')
    mmd.levels=3;mmd.sculpt_levels=2
    required(bpy.ops.object.mode_set(mode='SCULPT'),'Sculpt dopo Subdivide')
    bpy.context.view_layer.update()
    print('[LAZY_TEST] geometric-subdivide END',flush=True)
    required(bpy.ops.ed.undo_push(message='Geometry level 4 color level retained checkpoint'),'Geometry checkpoint')


def attribute(action, name, new_name=''):
    required(bpy.ops.sculpt.multires_polypaint_attribute(
        'EXEC_DEFAULT', True, action=action, name=name, new_name=new_name), action+' '+name)
    bpy.context.view_layer.update()

def paint_color(rgb, point):
    bpy.context.scene.tool_settings.sculpt.brush.color=rgb
    stroke(point)

def undo(label):
    required(bpy.ops.ed.undo(), label)

def redo(label):
    required(bpy.ops.ed.redo(), label)

steps=[
 lambda:attribute('SELECT','SkinColor'),
 lambda:save('before-a'),
 lambda:(paint_color((.85,.04,.015),(-.18,0.,0.)),save('after-a')),
 lambda:(undo('A stroke'),save('undo-a')),
 lambda:(redo('A stroke'),save('redo-a')),
 lambda:(attribute('ADD','Mask'),save('before-b')),
 lambda:(paint_color((.015,.15,.85),(.25,.12,0.)),save('after-b')),
 lambda:(undo('B stroke'),save('undo-b')),
 lambda:(redo('B stroke'),save('redo-b')),
 lambda:(attribute('SELECT','SkinColor'),save('select-a')),
 lambda:(undo('Select A'),save('select-undo')),
 lambda:(redo('Select A'),save('select-redo')),
 lambda:change_level(3),lambda:save('higher-before'),
 lambda:(paint_color((.7,.04,.1),(0.,-.2,0.)),save('higher-after')),
 lambda:(undo('Higher A Paint'),save('higher-undo')),
 lambda:(redo('Higher A Paint'),save('higher-redo')),
 lambda:undo('Higher stroke again'),
 lambda:(undo('A master promotion'),save('promotion-undo')),
 lambda:(redo('A master promotion'),save('promotion-redo')),
 lambda:(redo('Higher stroke across boundary'),save('boundary-redo')),
 lambda:(attribute('RENAME','SkinColor','Albedo'),save('renamed')),
 lambda:(undo('Rename A'),save('rename-undo')),
 lambda:(redo('Rename A'),save('rename-redo')),
 lambda:(attribute('REMOVE','Mask'),save('removed-b')),
 lambda:(undo('Remove B'),save('remove-undo')),
 lambda:(redo('Remove B'),save('remove-redo')),
 lambda:(undo('Restore B'),save('restored-b')),
 lambda:(attribute('ADD','Roughness'),save('added-c')),
 lambda:(undo('Add C'),save('add-undo')),
 lambda:(redo('Add C'),save('add-redo')),
 lambda:(attribute('REMOVE','Roughness'),save('removed-active-c')),
 lambda:(undo('Remove active C'),save('remove-active-undo')),
 lambda:(redo('Remove active C'),save('remove-active-redo')),
 delete_and_subdivide,lambda:save('after-subdivide'),
]
index=-1
fixture_initialized=False

def finish():
    (out/'ui-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    bpy.ops.wm.quit_blender()

def tick():
    global index,fixture_initialized
    try:
        window,area,region,_=viewport()
        with bpy.context.temp_override(window=window,area=area,region=region):
            if index==-1:
                if not fixture_initialized:
                    setup();fixture_initialized=True;return .35
                if not validation_brush.activate(report):
                    return .35
                required(bpy.ops.ed.undo_push(message='Interactive safety fixture ready'),'Initial checkpoint')
                index=0;return .8
            if index<len(steps):steps[index]();index+=1;return .35
        report['status']='PASS';finish();return None
    except Exception:
        report['status']='FAIL';report['error']=traceback.format_exc();traceback.print_exc();finish();return None

bpy.app.timers.register(tick,first_interval=1.2)
