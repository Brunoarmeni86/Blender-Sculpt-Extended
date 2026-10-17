"""B07: fresh-scene GUI test for animated layer Strength in Sculpt Mode."""
import bpy
import json
import traceback
from pathlib import Path

ROOT=Path(__file__).parent
RESULT=ROOT/'risultati-strength-animation-b07.json'

def report(data):
    RESULT.write_text(json.dumps(data,indent=2),encoding='utf-8')
    print('[STRENGTH_ANIMATION_B07] '+json.dumps(data),flush=True)

def run():
    result={'status':'RUNNING'}
    try:
        assert not bpy.app.background
        win=bpy.context.window or next(iter(bpy.context.window_manager.windows))
        area=next(a for a in win.screen.areas if a.type=='VIEW_3D')
        region=next(r for r in area.regions if r.type=='WINDOW')
        with bpy.context.temp_override(window=win,area=area,region=region):
            bpy.ops.object.select_all(action='SELECT')
            bpy.ops.object.delete(use_global=False)
            bpy.ops.mesh.primitive_uv_sphere_add(segments=8,ring_count=4)
            ob=bpy.context.object
            m=ob.modifiers.new('Multires','MULTIRES')
            for _ in range(2):
                assert 'FINISHED' in bpy.ops.object.multires_subdivide('EXEC_DEFAULT',True,modifier=m.name)
            m.levels=m.sculpt_levels=m.total_levels
            bpy.ops.object.mode_set('EXEC_DEFAULT',True,mode='SCULPT')
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_add('EXEC_DEFAULT',True)
            assert len(m.sculpt_layers)==1
            layer=m.sculpt_layers[0]
            layer.strength=.25
            assert layer.keyframe_insert(data_path='strength',frame=1)
            layer.strength=.75
            assert layer.keyframe_insert(data_path='strength',frame=10)
            # The keyframe path belongs to Object, not the modifier's evaluated ID.
            assert ob.animation_data and ob.animation_data.action
            for frame,expected in [(1,.25),(10,.75),(5,None),(1,.25)]:
                m.sculpt_layer_recording=True
                bpy.context.scene.frame_set(frame)
                bpy.context.view_layer.update()
                if expected is None:
                    assert .25<layer.strength<.75,(frame,layer.strength)
                else:
                    assert abs(layer.strength-expected)<.015,(frame,layer.strength)
                assert m.sculpt_layer_recording,(frame,'REC switched off by animation')
            result['frames_checked']=4
            # Explicit user edit still has the old transaction semantics.
            m.sculpt_layer_recording=True
            layer.strength=.5
            assert abs(layer.strength-.5)<1e-6
            assert not m.sculpt_layer_recording,'Interactive strength no longer ends REC'
            result['interactive_strength_keeps_transaction']=True
        result.update(status='PASS',scope='Action keyframes in Sculpt; REC maintained across frame changes; interactive edit remains transactional. Driver/NLA/pending live stroke not tested.')
    except Exception:
        result.update(status='FAIL',traceback=traceback.format_exc())
        traceback.print_exc()
    report(result)

report({'status':'STARTING'})
bpy.app.timers.register(run,first_interval=.8)
