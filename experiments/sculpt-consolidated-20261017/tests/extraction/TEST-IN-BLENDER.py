"""Factory-scene integration check. GUI required for native mode/Undo transitions."""
import bpy
import json
import traceback
from pathlib import Path

ROOT = Path(__file__).parent
RESULT = ROOT / 'risultati-test-estrazione.json'
if bpy.app.background:
    raise RuntimeError('Usa VERIFICA.ps1: il test di Undo richiede la finestra Blender')


def coordinates(mesh):
    return tuple(tuple(v.co) for v in mesh.vertices)


def finish(result):
    RESULT.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('[HR_EXTRACT_TEST] ' + json.dumps(result), flush=True)


def run_test():
    result = {'status': 'RUNNING'}
    try:
        win = bpy.context.window
        area = next(a for a in win.screen.areas if a.type == 'VIEW_3D')
        region = next(r for r in area.regions if r.type == 'WINDOW')
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.context.preferences.edit.use_global_undo = True
            bpy.context.preferences.edit.undo_steps = 32
            bpy.ops.object.select_all(action='SELECT')
            bpy.ops.object.delete(use_global=False)
            mesh = bpy.data.meshes.new('HR_Extract_Test_Base')
            mesh.from_pydata([(0,0,0),(1,0,0),(1,1,0),(0,1,0),
                              (2,0,0),(3,0,0),(3,1,0),(2,1,0)], [],
                             [(0,1,2,3),(4,5,6,7)])
            source = bpy.data.objects.new('HR_Extract_Source', mesh)
            bpy.context.collection.objects.link(source)
            source.select_set(True)
            bpy.context.view_layer.objects.active = source
            ids = mesh.attributes.new('.sculpt_face_set', 'INT', 'FACE')
            ids.data[0].value, ids.data[1].value = 2, 3
            base_coordinates = coordinates(mesh)
            mmd = source.modifiers.new('Multires', 'MULTIRES')
            for _ in range(2):
                assert 'FINISHED' in bpy.ops.object.multires_subdivide('EXEC_DEFAULT', True, modifier=mmd.name)
            mmd.levels = mmd.sculpt_levels = mmd.total_levels
            bpy.ops.object.mode_set('EXEC_DEFAULT', True, mode='SCULPT')
            bpy.context.view_layer.update()
            assert 'FINISHED' in bpy.ops.sculpt.multires_face_sets_from_base('EXEC_DEFAULT', True)
            assert 'FINISHED' in bpy.ops.sculpt.multires_layer_add('EXEC_DEFAULT', True)
            mmd = source.modifiers['Multires']
            assert len(mmd.sculpt_layers) == 1
            mmd.sculpt_layers[0].strength = 0.0
            mmd.sculpt_layer_recording = False
            bpy.context.view_layer.update()
            bpy.ops.sculpt.multires_base_fingerprint(label='EXTRACT_V05_BEFORE', include_layer_payloads=True, include_surface=False)
            assert 'FINISHED' in bpy.ops.sculpt.multires_face_sets_hr_mask(
                'EXEC_DEFAULT', True, active_face_set=2, mode='REPLACE')
            bpy.context.view_layer.update()
            assert 'FINISHED' in bpy.ops.sculpt.multires_hr_extract_v05(
                'EXEC_DEFAULT', True, source='MASK', max_vertices=10000, max_faces=10000)
            extracted = bpy.context.active_object
            assert extracted != source and extracted.type == 'MESH'
            assert len(extracted.data.vertices) == 25, len(extracted.data.vertices)
            assert len(extracted.data.polygons) == 16, len(extracted.data.polygons)
            assert all(poly.normal.z > 0.99 for poly in extracted.data.polygons)
            output_name = extracted.name
            output_coordinates = coordinates(extracted.data)
            source = bpy.data.objects['HR_Extract_Source']
            assert coordinates(source.data) == base_coordinates
            mmd = source.modifiers['Multires']
            assert len(mmd.sculpt_layers) == 1 and mmd.sculpt_layers[0].strength == 0.0
            assert not mmd.sculpt_layer_recording
            with bpy.context.temp_override(active_object=source, object=source):
                bpy.ops.sculpt.multires_base_fingerprint(label='EXTRACT_V05_AFTER', include_layer_payloads=True, include_surface=False)
            assert 'FINISHED' in bpy.ops.ed.undo()
            assert bpy.data.objects.get(output_name) is None
            assert 'FINISHED' in bpy.ops.ed.redo()
            extracted = bpy.data.objects[output_name]
            assert coordinates(extracted.data) == output_coordinates
            source = bpy.data.objects['HR_Extract_Source']
            bpy.context.view_layer.objects.active = source
            bpy.ops.object.select_all(action='DESELECT')
            source.select_set(True)
            bpy.ops.object.mode_set('EXEC_DEFAULT', True, mode='SCULPT')
            bpy.context.view_layer.update()
            before_objects = set(bpy.data.objects.keys())
            try:
                status = bpy.ops.sculpt.multires_hr_extract_v05(
                    'EXEC_DEFAULT', True, source='MASK', max_vertices=4, max_faces=10000)
            except RuntimeError:
                status = {'CANCELLED'}
            assert 'CANCELLED' in status and set(bpy.data.objects.keys()) == before_objects
            assert source.mode == 'SCULPT'
            assert 'FINISHED' in bpy.ops.sculpt.multires_hr_extract_v05(
                'EXEC_DEFAULT', True, source='FACE_SET', active_face_set=3,
                max_vertices=10000, max_faces=10000)
            other = bpy.context.active_object
            assert len(other.data.vertices) == 25 and len(other.data.polygons) == 16
            assert all(v.co.x >= 1.99 for v in other.data.vertices)
            path = ROOT / 'demo-estrazione-hr-v05.blend'
            bpy.ops.wm.save_as_mainfile(filepath=str(path))
        bpy.ops.wm.open_mainfile(filepath=str(path))
        assert bpy.data.objects.get(output_name) is not None
        assert coordinates(bpy.data.objects[output_name].data) == output_coordinates
        source = bpy.data.objects['HR_Extract_Source']
        assert coordinates(source.data) == base_coordinates
        assert source.modifiers['Multires'].sculpt_layers[0].strength == 0.0
        result.update(status='PASS', mask_extract=True, face_set_extract=True,
                      welded_vertices=25, quads=16, source_cage_unchanged=True,
                      zero_strength_layer_retained=True, undo_redo=True,
                      output_limit_rejected=True, save_reopen=True)
    except Exception:
        result.update(status='FAIL', traceback=traceback.format_exc())
        traceback.print_exc()
    finish(result)
    return None

finish({'status': 'STARTING', 'scope': 'Small factory scene; Draw brush requires a separate manual check'})
bpy.app.timers.register(run_test, first_interval=0.8)
