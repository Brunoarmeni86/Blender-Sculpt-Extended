"""Native GUI regression fixture; requires the already integrated Detail Transfer V01.
Does not load user files. Tests only the merge/group/Bake cluster.
"""
import bpy
import json
import traceback
from array import array
from pathlib import Path
ROOT=Path(__file__).parent
RESULT=ROOT/'risultati-sicurezza-v01.json'

def finish(result):
    RESULT.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('[SCULPT_SAFETY_V01] '+json.dumps(result),flush=True)

def coordinates(ob):
    bpy.context.view_layer.update()
    ev=ob.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh()
    try:
        vals=array('f',[0.0])*(len(mesh.vertices)*3);mesh.vertices.foreach_get('co',vals)
        return vals
    finally:ev.to_mesh_clear()

def compare(a,b):
    assert len(a)==len(b),(len(a),len(b))
    error=max((abs(x-y)for x,y in zip(a,b)),default=0.0)
    assert error<4e-5,error
    return error

def mode(value):
    if bpy.context.object.mode!=value:
        assert 'FINISHED' in bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode=value)

def fixture(name,values):
    if bpy.context.object is not None:mode('OBJECT')
    bpy.ops.object.select_all(action='DESELECT')
    mesh=bpy.data.meshes.new(name+'_Base')
    mesh.from_pydata([(0,0,0),(1,0,0),(1,1,0),(0,1,0)],[],[(0,1,2,3)])
    ob=bpy.data.objects.new(name,mesh);bpy.context.collection.objects.link(ob)
    ob.select_set(True);bpy.context.view_layer.objects.active=ob
    m=ob.modifiers.new('Multires','MULTIRES')
    for _ in range(2):bpy.ops.object.multires_subdivide('EXEC_DEFAULT',True,modifier=m.name)
    m.levels=m.sculpt_levels=m.total_levels
    for index,value in enumerate(values):
        if value==0:
            mode('SCULPT');assert 'FINISHED' in bpy.ops.sculpt.multires_layer_add('EXEC_DEFAULT',True);mode('OBJECT')
        else:
            ev=ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
            pm=bpy.data.meshes.new_from_object(ev)
            origin=array('f',[0.0])*(len(pm.vertices)*3);pm.vertices.foreach_get('co',origin)
            delta=array('f',[0.0])*len(origin);shown=array('f',origin)
            for i in range(len(pm.vertices)):delta[3*i+2]=value;shown[3*i+2]+=value
            for attr,vals in [('detail_transfer_origin',origin),('detail_transfer_delta',delta)]:
                a=pm.attributes.new(attr,'FLOAT_VECTOR','POINT');a.data.foreach_set('vector',vals)
            pm.vertices.foreach_set('co',shown);pm.update()
            preview=bpy.data.objects.new(name+'_Input',pm);bpy.context.collection.objects.link(preview)
            bpy.context.view_layer.update()
            assert 'FINISHED' in bpy.ops.sculpt.multires_detail_transfer_import_v01('EXEC_DEFAULT',True,preview_object=preview.name,layer_name='Layer_'+str(index))
            preview.hide_set(True)
        ob.modifiers['Multires'].sculpt_layers[-1].name='Layer_'+str(index)
    return ob

def select_layer(index):
    m=bpy.context.object.modifiers['Multires']
    if m.active_sculpt_layer_index!=index:
        assert 'FINISHED' in bpy.ops.sculpt.multires_layer_set_active('EXEC_DEFAULT',True,index=index)


def world_coordinates(ob):
    from mathutils import Vector
    values=coordinates(ob);result=[]
    for i in range(0,len(values),3):result.extend(ob.matrix_world @ Vector(values[i:i+3]))
    return result

def rejected(call):
    try:return 'CANCELLED' in call()
    except RuntimeError as error:
        text=str(error)
        assert 'Sculpt Layers support Apply Scale' in text or 'requires explicit migration' in text,text
        return True

def run_test():
    result={'status':'RUNNING'}
    try:
        assert not bpy.app.background,'Usa VERIFICA.ps1 con GUI'
        win=bpy.context.window or next(iter(bpy.context.window_manager.windows),None)
        area=next(a for a in win.screen.areas if a.type=='VIEW_3D')
        region=next(r for r in area.regions if r.type=='WINDOW')
        with bpy.context.temp_override(window=win,area=area,region=region):
            bpy.context.preferences.edit.use_global_undo=True
            bpy.context.preferences.edit.undo_steps=32
            bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
            ob=fixture('Scale_Layers',[0,.125,.0625])
            m=ob.modifiers['Multires'];m.sculpt_layers[1].strength=.5
            m.sculpt_layers[2].visible=False
            ob.scale=(2,2,2);bpy.context.view_layer.update()
            before=world_coordinates(ob)
            # Capture the hidden contribution before applying scale, then restore visibility.
            m.sculpt_layers[2].visible=True;all_before=world_coordinates(ob)
            m.sculpt_layers[2].visible=False
            bpy.ops.ed.undo_push(message='Before layer scale')
            assert 'FINISHED' in bpy.ops.object.transform_apply('EXEC_DEFAULT',True,location=False,rotation=False,scale=True)
            result['scale_world_error']=compare(world_coordinates(ob),before)
            assert tuple(ob.scale)==(1,1,1)
            assert 'FINISHED' in bpy.ops.ed.undo()
            ob=bpy.data.objects['Scale_Layers'];compare(world_coordinates(ob),before)
            assert tuple(ob.scale)==(2,2,2)
            assert 'FINISHED' in bpy.ops.ed.redo()
            ob=bpy.data.objects['Scale_Layers'];compare(world_coordinates(ob),before)
            assert tuple(ob.scale)==(1,1,1)
            result['undo_redo']=True
            ob.modifiers['Multires'].sculpt_layers[2].visible=True
            result['hidden_layer_error']=compare(world_coordinates(ob),all_before)
            # A later invalid selected object must prevent changes to the entire selection.
            bad=fixture('Scale_Rejected',[.125]);bad.scale=(1,2,1)
            ob.select_set(True);ob.scale=(2,2,2)
            orig=world_coordinates(ob);bad_orig=world_coordinates(bad)
            assert rejected(lambda:bpy.ops.object.transform_apply('EXEC_DEFAULT',True,location=False,rotation=False,scale=True))
            compare(world_coordinates(ob),orig);compare(world_coordinates(bad),bad_orig)
            assert tuple(ob.scale)==(2,2,2) and tuple(bad.scale)==(1,2,1)
            result['nonuniform_selection_unchanged']=True
            names=set(bpy.data.objects.keys())
            assert rejected(lambda:bpy.ops.object.join('EXEC_DEFAULT',True))
            assert set(bpy.data.objects.keys())==names
            compare(world_coordinates(ob),orig);compare(world_coordinates(bad),bad_orig)
            result['join_guard']=True
            # Native mesh Join without persistent masters stays available.
            bpy.ops.object.select_all(action='DESELECT')
            bpy.ops.mesh.primitive_cube_add();a=bpy.context.object
            bpy.ops.mesh.primitive_cube_add();b=bpy.context.object;a.select_set(True)
            assert 'FINISHED' in bpy.ops.object.join('EXEC_DEFAULT',True)
            assert len(bpy.context.object.data.vertices)==16
            result['ordinary_join']=True
            expected={name:world_coordinates(bpy.data.objects[name]) for name in ['Scale_Layers','Scale_Rejected']}
            path=ROOT/'demo-sicurezza-v01.blend';bpy.ops.wm.save_as_mainfile(filepath=str(path))
        bpy.ops.wm.open_mainfile(filepath=str(path))
        win=bpy.context.window or next(iter(bpy.context.window_manager.windows),None)
        area=next(a for a in win.screen.areas if a.type=='VIEW_3D');region=next(r for r in area.regions if r.type=='WINDOW')
        with bpy.context.temp_override(window=win,area=area,region=region):
            for name,values in expected.items():compare(world_coordinates(bpy.data.objects[name]),values)
        result.update(status='PASS',save_reopen=True,scope='Small native HR geometry fixtures; L0 tested separately with extracted C++; no dense mesh benchmark')
    except Exception:
        result.update(status='FAIL',traceback=traceback.format_exc());traceback.print_exc()
    finish(result);return None

finish({'status':'STARTING'})
bpy.app.timers.register(run_test,first_interval=.8)
