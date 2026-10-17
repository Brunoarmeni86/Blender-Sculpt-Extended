"""Native GUI regression fixture; requires the already integrated Detail Transfer V01.
Does not load user files. Tests only the merge/group/Bake cluster.
"""
import bpy
import json
import traceback
from array import array
from pathlib import Path
ROOT=Path(__file__).parent
RESULT=ROOT/'risultati-fusioni-v01.json'

def finish(result):
    RESULT.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('[MERGE_GROUPS_TEST_V01] '+json.dumps(result),flush=True)

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

def run_test():
    result={'status':'RUNNING'}
    try:
        assert not bpy.app.background,'Usa VERIFICA.ps1 con GUI'
        bpy.ops.sculpt.multires_detail_transfer_import_v01.get_rna_type()
        win=bpy.context.window or next(iter(bpy.context.window_manager.windows),None)
        area=next(a for a in win.screen.areas if a.type=='VIEW_3D')
        region=next(r for r in area.regions if r.type=='WINDOW')
        with bpy.context.temp_override(window=win,area=area,region=region):
            bpy.context.preferences.edit.use_global_undo=True;bpy.context.preferences.edit.undo_steps=32
            bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
            # A sparse first layer must receive the second layer's detail.
            ob=fixture('Merge_Flatten',[0,.125]);expected=coordinates(ob)
            mode('SCULPT')
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_flatten('EXEC_DEFAULT',True)
            assert len(bpy.context.object.modifiers['Multires'].sculpt_layers)==1
            assert 'FINISHED' in bpy.ops.ed.undo()
            assert len(bpy.data.objects['Merge_Flatten'].modifiers['Multires'].sculpt_layers)==2
            assert 'FINISHED' in bpy.ops.ed.redo()
            ob=bpy.data.objects['Merge_Flatten'];bpy.context.view_layer.objects.active=ob
            assert len(ob.modifiers['Multires'].sculpt_layers)==1
            mode('OBJECT');result['flatten_error']=compare(coordinates(ob),expected)
            result['flatten_undo_redo']=True
            # Merge Down must select the real destination, not the external third layer.
            ob=fixture('Merge_Down',[0,.125,.0625]);expected=coordinates(ob)
            mode('SCULPT');select_layer(0)
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_merge('EXEC_DEFAULT',True)
            m=bpy.context.object.modifiers['Multires']
            assert len(m.sculpt_layers)==2 and m.active_sculpt_layer_index==0
            assert m.sculpt_layers[0].name=='Layer_1' and m.sculpt_layers[1].name=='Layer_2'
            mode('OBJECT');result['merge_down_error']=compare(coordinates(ob),expected)
            # Whole-group publication with a separate layer after it.
            ob=fixture('Merge_Group',[.03125,.0625,.125,.25]);expected=coordinates(ob)
            mode('SCULPT');select_layer(0)
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_group_with_below('EXEC_DEFAULT',True)
            select_layer(2)
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_group_add_active('EXEC_DEFAULT',True,group_id=1)
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_group_merge('EXEC_DEFAULT',True,group_id=1)
            m=bpy.context.object.modifiers['Multires']
            assert len(m.sculpt_layers)==2 and m.active_sculpt_layer_index==0
            assert m.sculpt_layers[1].name=='Layer_3'
            mode('OBJECT');result['group_merge_error']=compare(coordinates(ob),expected)
            # Remove the middle group row, then merge the remaining group.
            ob=fixture('Merge_Group_Remove',[.03125,.0625,.125,.25]);expected=coordinates(ob)
            mode('SCULPT');select_layer(0)
            bpy.ops.sculpt.multires_layer_group_with_below('EXEC_DEFAULT',True)
            select_layer(2);bpy.ops.sculpt.multires_layer_group_add_active('EXEC_DEFAULT',True,group_id=1)
            select_layer(1)
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_group_remove('EXEC_DEFAULT',True)
            assert bpy.context.object.modifiers['Multires'].active_sculpt_layer_index==2
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_group_merge('EXEC_DEFAULT',True,group_id=1)
            assert len(bpy.context.object.modifiers['Multires'].sculpt_layers)==3
            mode('OBJECT');result['group_remove_merge_error']=compare(coordinates(ob),expected)
            # Merge a hidden folder into a visible one, preserving individual visibility.
            ob=fixture('Merge_Visibility',[.03125,.0625]);mode('SCULPT')
            select_layer(0);bpy.ops.sculpt.multires_layer_group_create('EXEC_DEFAULT',True)
            select_layer(1);bpy.ops.sculpt.multires_layer_group_create('EXEC_DEFAULT',True)
            bpy.ops.sculpt.multires_layer_group_visibility('EXEC_DEFAULT',True,group_id=2)
            assert not bpy.context.object.modifiers['Multires'].sculpt_layers[1].visible
            select_layer(0);bpy.ops.sculpt.multires_layer_group_with_below('EXEC_DEFAULT',True)
            assert bpy.context.object.modifiers['Multires'].sculpt_layers[1].visible
            bpy.ops.sculpt.multires_layer_group_visibility('EXEC_DEFAULT',True,group_id=1)
            bpy.ops.sculpt.multires_layer_group_visibility('EXEC_DEFAULT',True,group_id=1)
            assert bpy.context.object.modifiers['Multires'].sculpt_layers[1].visible
            result['folder_visibility_preserved']=True;mode('OBJECT')
            # Legacy Bake receives sparse layers through the validated shared HR path.
            ob=fixture('Merge_Bake',[0,.125]);expected=coordinates(ob)
            mode('SCULPT')
            assert 'FINISHED' in bpy.ops.sculpt.multires_base_capture('EXEC_DEFAULT',True)
            mode('OBJECT');compare(coordinates(ob),expected)
            for layer in ob.modifiers['Multires'].sculpt_layers:layer.strength=-1.0
            result['legacy_sparse_bake_error']=compare(coordinates(ob),expected)
            saved_expected={name:coordinates(bpy.data.objects[name]) for name in ['Merge_Flatten','Merge_Down','Merge_Group','Merge_Group_Remove','Merge_Bake']}
            path=ROOT/'demo-fusioni-v01.blend';bpy.ops.wm.save_as_mainfile(filepath=str(path))
        bpy.ops.wm.open_mainfile(filepath=str(path))
        for name,expected in saved_expected.items():
            # Evaluated coordinates need a valid window context after file load.
            win=bpy.context.window or next(iter(bpy.context.window_manager.windows),None)
            area=next(a for a in win.screen.areas if a.type=='VIEW_3D');region=next(r for r in area.regions if r.type=='WINDOW')
            with bpy.context.temp_override(window=win,area=area,region=region):compare(coordinates(bpy.data.objects[name]),expected)
        result.update(status='PASS',save_reopen=True,scope='Small native fixtures; geometry-layer merge/group/Bake only; not the HR extraction test')
    except Exception:
        result.update(status='FAIL',traceback=traceback.format_exc());traceback.print_exc()
    finish(result);return None

finish({'status':'STARTING'})
bpy.app.timers.register(run_test,first_interval=.8)
