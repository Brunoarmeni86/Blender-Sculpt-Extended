"""GUI regression for concurrent Face Set Undo master initialization on Multires."""
import bpy
import json
import traceback
from pathlib import Path

ROOT=Path(__file__).parent
RESULT=ROOT/'risultati-faceset-undo-k01.json'

def report(data):
    RESULT.write_text(json.dumps(data,indent=2),encoding='utf-8')
    print('[FACESET_UNDO_K01] '+json.dumps(data),flush=True)

def fixture(name):
    bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='OBJECT') if bpy.context.object and bpy.context.object.mode!='OBJECT' else None
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=49,y_subdivisions=49,size=3)
    ob=bpy.context.object
    ob.name=name
    m=ob.modifiers.new('Multires','MULTIRES')
    for _ in range(2):
        assert 'FINISHED' in bpy.ops.object.multires_subdivide('EXEC_DEFAULT',True,modifier=m.name)
    m.levels=m.sculpt_levels=m.total_levels
    bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='SCULPT')
    bpy.context.view_layer.update()
    return ob,m

def run():
    result={'status':'RUNNING'}
    try:
        assert not bpy.app.background
        win=bpy.context.window or next(iter(bpy.context.window_manager.windows))
        area=next(a for a in win.screen.areas if a.type=='VIEW_3D')
        region=next(r for r in area.regions if r.type=='WINDOW')
        with bpy.context.temp_override(window=win,area=area,region=region):
            bpy.context.preferences.edit.use_global_undo=True
            bpy.context.preferences.edit.undo_steps=32
            bpy.ops.object.select_all(action='SELECT')
            bpy.ops.object.delete(use_global=False)
            for name,operator in [('K01_ParallelNode','create'),('K01_BatchNode','init')]:
                ob,m=fixture(name)
                if operator=='create':
                    status=bpy.ops.sculpt.face_sets_create('EXEC_DEFAULT',True,mode='ALL')
                else:
                    status=bpy.ops.sculpt.face_sets_init('EXEC_DEFAULT',True,mode='LOOSE_PARTS')
                assert 'FINISHED' in status,(operator,status)
                assert 'FINISHED' in bpy.ops.ed.undo()
                assert 'FINISHED' in bpy.ops.ed.redo()
                ob=bpy.data.objects.get(name)
                assert ob is not None and ob.modifiers.get('Multires') is not None
                result[operator+'_undo_redo']=True
            result.update(status='PASS',scope='49x49 factory grids with no prior Face Set operation; global parallel push_node and batch push_nodes paths; two Undo/Redo cycles. RNA does not expose the HR master; race absence cannot be proven by one run.')
    except Exception:
        result.update(status='FAIL',traceback=traceback.format_exc())
        traceback.print_exc()
    report(result)

report({'status':'STARTING'})
bpy.app.timers.register(run,first_interval=.8)
