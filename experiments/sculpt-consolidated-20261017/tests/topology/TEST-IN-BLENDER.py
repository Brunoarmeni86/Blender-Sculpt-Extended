"""Small native integration test. Requires our already integrated Detail Transfer V01.
No user file is loaded. GUI is needed for Undo and Edit Mode transitions.
"""
import bpy
import json
import traceback
from pathlib import Path
from array import array
ROOT=Path(__file__).parent
RESULT=ROOT/'risultati-binding-v02.json'
NAME='Binding_V02_Target'

def finish(result):
    RESULT.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('[CORNER_BINDING_TEST_V02] '+json.dumps(result),flush=True)

def active():
    ob=bpy.data.objects[NAME]
    bpy.context.view_layer.objects.active=ob
    return ob

def heights(ob):
    bpy.context.view_layer.update()
    evaluated=ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh=evaluated.to_mesh()
    try:
        assert len(mesh.vertices)>0
        return [float(v.co.z) for v in mesh.vertices]
    finally:evaluated.to_mesh_clear()

def assert_detail(ob):
    z=heights(ob)
    assert max(abs(v-.125)for v in z)<5e-5,(min(z),max(z))
    m=ob.modifiers['Multires']
    assert len(m.sculpt_layers)==1
    assert abs(m.sculpt_layers[0].strength-1)<1e-6

def corner_ids(ob):
    attr=ob.data.attributes.get('.sculpt_layer_corner_id_v2')
    assert attr is not None
    return [v.value for v in attr.data]

def subdivide(ob):
    m=ob.modifiers['Multires'];before=m.total_levels
    result=bpy.ops.object.multires_subdivide('EXEC_DEFAULT',True,modifier=m.name,mode='CATMULL_CLARK')
    assert 'FINISHED' in result,result
    assert ob.modifiers['Multires'].total_levels==before+1,'Subdivision refused or no level added'
    m=ob.modifiers['Multires'];m.levels=m.total_levels
    assert_detail(ob)

def run_test():
    result={'status':'RUNNING'}
    try:
        assert not bpy.app.background,'Usa VERIFICA.ps1 con finestra Blender'
        bpy.ops.sculpt.multires_detail_transfer_import_v01.get_rna_type()
        win=bpy.context.window or next(iter(bpy.context.window_manager.windows), None)
        area=next(a for a in win.screen.areas if a.type=='VIEW_3D')
        region=next(r for r in area.regions if r.type=='WINDOW')
        with bpy.context.temp_override(window=win,area=area,region=region):
            bpy.context.preferences.edit.use_global_undo=True
            bpy.context.preferences.edit.undo_steps=32
            bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
            mesh=bpy.data.meshes.new('Binding_V02_Base')
            mesh.from_pydata([(0,0,0),(1,0,0),(1,1,0),(0,1,0),
                              (2,0,0),(3,0,0),(3,1,0),(2,1,0)],[],[(0,1,2,3),(4,5,6,7)])
            ob=bpy.data.objects.new(NAME,mesh);bpy.context.collection.objects.link(ob)
            ob.select_set(True);bpy.context.view_layer.objects.active=ob
            m=ob.modifiers.new('Multires','MULTIRES')
            for _ in range(2):
                bpy.ops.object.multires_subdivide('EXEC_DEFAULT',True,modifier=m.name)
            m.levels=m.total_levels
            ev=ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
            preview_mesh=bpy.data.meshes.new_from_object(ev)
            origin=array('f',[0.0])*(len(preview_mesh.vertices)*3)
            preview_mesh.vertices.foreach_get('co',origin)
            delta=array('f',[0.0])*len(origin);shown=array('f',origin)
            for i in range(len(preview_mesh.vertices)):
                if origin[3*i]>1.5:
                    delta[3*i+2]=.125;shown[3*i+2]+=.125
            for name,values in [('detail_transfer_origin',origin),('detail_transfer_delta',delta)]:
                attr=preview_mesh.attributes.new(name,'FLOAT_VECTOR','POINT');attr.data.foreach_set('vector',values)
            preview_mesh.vertices.foreach_set('co',shown);preview_mesh.update()
            preview=bpy.data.objects.new('Binding_V02_Preview',preview_mesh)
            bpy.context.collection.objects.link(preview);bpy.context.view_layer.update()
            assert 'FINISHED' in bpy.ops.sculpt.multires_detail_transfer_import_v01(
                'EXEC_DEFAULT',True,preview_object=preview.name,layer_name='Binding Test Detail')
            preview.hide_set(True)
            before_ids=corner_ids(ob)
            assert len(before_ids)==8,before_ids
            for v in ob.data.vertices:v.select=False
            for p in ob.data.polygons:p.select=False
            ob.data.polygons[0].select=True
            bpy.context.tool_settings.mesh_select_mode=(False,False,True)
            bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='EDIT')
            assert 'FINISHED' in bpy.ops.mesh.delete('EXEC_DEFAULT',True,type='FACE')
            bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='OBJECT')
            ob=active();survivors=corner_ids(ob)
            assert len(survivors)==4 and survivors==before_ids[4:],(before_ids,survivors)
            assert survivors!=[1,2,3,4],survivors
            assert_detail(ob);result['edit_preserves_detail']=True
            subdivide(ob);result['subdivide_noncanonical']=True
            total=ob.modifiers['Multires'].total_levels
            assert 'FINISHED' in bpy.ops.ed.undo()
            ob=active();assert ob.modifiers['Multires'].total_levels==total-1
            assert_detail(ob)
            assert 'FINISHED' in bpy.ops.ed.redo()
            ob=active();assert ob.modifiers['Multires'].total_levels==total
            assert_detail(ob);result['undo_redo_subdivide']=True
            path=ROOT/'demo-binding-v02.blend'
            bpy.ops.wm.save_as_mainfile(filepath=str(path))
        # Do not keep a context override referencing the old screen across file load.
        bpy.ops.wm.open_mainfile(filepath=str(path))
        win=bpy.context.window or next(iter(bpy.context.window_manager.windows), None)
        area=next(a for a in win.screen.areas if a.type=='VIEW_3D')
        region=next(r for r in area.regions if r.type=='WINDOW')
        with bpy.context.temp_override(window=win,area=area,region=region):
            ob=active();assert corner_ids(ob)==survivors
            assert_detail(ob)
            subdivide(ob);result['save_reopen_then_subdivide']=True
            # Object duplication must deep-copy the binding; move the duplicate's
            # topology without changing the source's payload or association.
            bpy.ops.object.select_all(action='DESELECT');ob.select_set(True)
            bpy.ops.object.duplicate('EXEC_DEFAULT',True,linked=False)
            duplicate=bpy.context.active_object
            assert duplicate!=ob
            duplicate.modifiers['Multires'].levels=duplicate.modifiers['Multires'].total_levels
            assert_detail(duplicate);subdivide(duplicate);assert_detail(ob)
            result['duplicate_then_subdivide']=True
        result.update(status='PASS',scope='Small native geometry-layer fixture; other companions and legacy migration require separate checks')
    except Exception:
        result.update(status='FAIL',traceback=traceback.format_exc());traceback.print_exc()
    finish(result)
    return None

finish({'status':'STARTING'})
bpy.app.timers.register(run_test,first_interval=0.8)
