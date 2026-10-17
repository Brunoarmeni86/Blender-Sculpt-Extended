"""Native GUI regression for dynamic Undo accounting. Fresh scene, no user files."""
import bpy
import json
import traceback
from array import array
from pathlib import Path
ROOT=Path(__file__).parent
RESULT=ROOT/'risultati-undo-memoria-v01.json'

def report(result):
    RESULT.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('[UNDO_MEMORY_TEST_V01] '+json.dumps(result),flush=True)

def positions(ob):
    vals=array('f',[0.0])*(len(ob.data.vertices)*3)
    ob.data.vertices.foreach_get('co',vals)
    return vals

def compare(a,b):
    assert len(a)==len(b)
    error=max((abs(x-y)for x,y in zip(a,b)),default=0.)
    assert error<5e-5,error
    return error

def audit():
    assert 'FINISHED' in bpy.ops.sculpt.undo_memory_audit_v01('EXEC_DEFAULT',require_position=True)

def run_test():
    result={'status':'RUNNING'}
    try:
        assert not bpy.app.background,'Usa VERIFICA.ps1 con finestra Blender'
        bpy.ops.sculpt.undo_memory_audit_v01.get_rna_type()
        win=bpy.context.window or next(iter(bpy.context.window_manager.windows),None)
        area=next(a for a in win.screen.areas if a.type=='VIEW_3D')
        region=next(r for r in area.regions if r.type=='WINDOW')
        with bpy.context.temp_override(window=win,area=area,region=region):
            bpy.context.preferences.edit.use_global_undo=True
            bpy.context.preferences.edit.undo_steps=32
            bpy.context.preferences.edit.undo_memory_limit=256
            bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
            bpy.ops.mesh.primitive_uv_sphere_add(segments=32,ring_count=16)
            ob=bpy.context.object;ob.name='UndoMemory_Mesh'
            bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='SCULPT')
            start_mouse=(region.width//2,region.height//2)
            worst=0.
            for i in range(3):
                before=positions(ob)
                assert 'FINISHED' in bpy.ops.sculpt.mesh_filter('EXEC_DEFAULT',True,type='INFLATE',strength=.03,start_mouse=start_mouse)
                after=positions(ob)
                assert max(abs(a-b)for a,b in zip(before,after))>1e-7,'Mesh filter produced no geometric change'
                audit()
                assert 'FINISHED' in bpy.ops.ed.undo()
                ob=bpy.data.objects['UndoMemory_Mesh'];worst=max(worst,compare(positions(ob),before));audit()
                assert 'FINISHED' in bpy.ops.ed.redo()
                ob=bpy.data.objects['UndoMemory_Mesh'];worst=max(worst,compare(positions(ob),after));audit()
            result.update(mesh_filters=3,mesh_undo_redo_error=worst)
            # Non-position step must retain the same metadata accounting contract.
            assert 'FINISHED' in bpy.ops.paint.mask_flood_fill('EXEC_DEFAULT',True,mode='VALUE',value=.5)
            audit();assert 'FINISHED' in bpy.ops.ed.undo();audit()
            assert 'FINISHED' in bpy.ops.ed.redo();audit();result['mask_undo_redo']=True
            bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='OBJECT')
            bpy.ops.object.select_all(action='DESELECT')
            bpy.ops.mesh.primitive_uv_sphere_add(segments=8,ring_count=4)
            ob=bpy.context.object;ob.name='UndoMemory_Multires'
            m=ob.modifiers.new('Multires','MULTIRES')
            for _ in range(3):assert 'FINISHED' in bpy.ops.object.multires_subdivide('EXEC_DEFAULT',True,modifier=m.name)
            m.levels=m.sculpt_levels=m.total_levels
            bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='SCULPT')
            for i in range(3):
                assert 'FINISHED' in bpy.ops.sculpt.mesh_filter('EXEC_DEFAULT',True,type='INFLATE',strength=.025,start_mouse=start_mouse)
                audit();assert 'FINISHED' in bpy.ops.ed.undo();audit()
                assert 'FINISHED' in bpy.ops.ed.redo();audit()
            result['multires_filter_undo_redo']=True
            bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='OBJECT')
            bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'demo-undo-memoria-v01.blend'))
        result.update(status='PASS',scope='Small Mesh/Multires filters and Mask Undo; audit checks completed compressed buffers; pending worker and budget trimming tested locally with real C++ bodies; no persistent layer/Cloth/Pose native stroke fixture')
    except Exception:
        result.update(status='FAIL',traceback=traceback.format_exc());traceback.print_exc()
    report(result);return None

report({'status':'STARTING'})
bpy.app.timers.register(run_test,first_interval=.8)
