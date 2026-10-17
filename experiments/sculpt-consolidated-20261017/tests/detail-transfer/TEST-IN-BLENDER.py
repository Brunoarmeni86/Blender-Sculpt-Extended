"""Small Windows integration scene. Run only on the patched build, factory startup."""
import bpy
import math
import sys
import runpy
import json
import hashlib
from pathlib import Path
from array import array

ROOT=Path(__file__).parent
if bpy.app.background:
    raise RuntimeError('Eseguire VERIFICA.ps1: questa prova Undo richiede la finestra Blender')
runpy.run_path(str(ROOT/'LOAD-TRANSFER.py'),run_name='detail_transfer_smoke_loader')
dt=sys.modules['sculpt_detail_transfer_v01']
bpy.context.preferences.edit.use_global_undo=True
bpy.context.preferences.edit.undo_steps=32
results={}

def evaluated(ob):
    dep=bpy.context.evaluated_depsgraph_get()
    ev=ob.evaluated_get(dep)
    mesh=ev.to_mesh()
    try:
        out=array('f',[0.0])*(3*len(mesh.vertices))
        mesh.vertices.foreach_get('co',out)
        return out
    finally: ev.to_mesh_clear()

def maximum_error(a,b):
    assert len(a)==len(b)
    return max((abs(x-y)for x,y in zip(a,b)),default=0)

def grid(name,cuts):
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=cuts,y_subdivisions=cuts,size=2)
    ob=bpy.context.object;ob.name=name
    return ob

def import_constant(target,value):
    ev=target.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh=bpy.data.meshes.new_from_object(ev)
    origin=array('f',[0.0])*(len(mesh.vertices)*3)
    mesh.vertices.foreach_get('co',origin)
    delta=array('f',[0.0])*len(origin)
    shown=array('f',origin)
    for i in range(len(mesh.vertices)):
        delta[3*i+2]=value;shown[3*i+2]+=value
    for name,vals in ((dt.ORIGIN,origin),(dt.DELTA,delta)):
        a=mesh.attributes.new(name,'FLOAT_VECTOR','POINT');a.data.foreach_set('vector',vals)
    mesh.vertices.foreach_set('co',shown);mesh.update()
    preview=bpy.data.objects.new('ProtectedLayer_Input',mesh)
    bpy.context.scene.collection.objects.link(preview)
    preview.matrix_world=target.matrix_world
    bpy.context.view_layer.update()
    bpy.context.view_layer.objects.active=target
    assert 'FINISHED' in bpy.ops.sculpt.multires_detail_transfer_import_v01('EXEC_DEFAULT', True,
        preview_object=preview.name,layer_name='Protected Existing Layer')
    preview.hide_set(True)

try:
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    reference=grid('DT_Reference',33)
    source=reference.copy();source.data=reference.data.copy();source.name='DT_Detailed'
    bpy.context.scene.collection.objects.link(source)
    for v in source.data.vertices:
        v.co.z=.025*math.sin(3*math.pi*v.co.x)*math.cos(2*math.pi*v.co.y)
    source.data.update()
    source_hash=hashlib.sha256(evaluated(source).tobytes()).hexdigest()
    reference_hash=hashlib.sha256(evaluated(reference).tobytes()).hexdigest()
    target=grid('DT_Target',5)
    m=target.modifiers.new('Multires','MULTIRES')
    for _ in range(2):bpy.ops.object.multires_subdivide(modifier=m.name,mode='CATMULL_CLARK')
    m.levels=m.total_levels
    base_coordinates=tuple(tuple(v.co)for v in target.data.vertices)
    import_constant(target,.01)
    m=dt.multires(target)
    assert len(m.sculpt_layers)==1
    m.sculpt_layers[0].strength=.37
    bpy.context.view_layer.update()
    before=evaluated(target)
    bpy.ops.sculpt.multires_base_fingerprint(label='DT_BEFORE',include_layer_payloads=True,include_surface=False)
    s=bpy.context.scene.detail_transfer_v01
    s.source_mode='PAIR';s.source=source;s.reference=reference;s.target=target
    s.max_distance=.1;s.max_angle=math.radians(60);s.distance_fade=False;s.mode='NORMAL'
    preview=dt.build_preview(s,bpy.context)
    expected=array('f',[0.0])*(len(preview.data.vertices)*3)
    preview.data.vertices.foreach_get('co',expected)
    assert len(source.data.vertices)!=len(expected)//3,'Test must use different source and target topology'
    assert 'FINISHED' in bpy.ops.sculpt.multires_detail_transfer_import_v01('EXEC_DEFAULT', True,
        preview_object=preview.name,layer_name='Transferred Test Detail')
    bpy.context.view_layer.update()
    after=evaluated(target)
    error=maximum_error(after,expected);assert error<3e-5,error
    m=dt.multires(target);assert len(m.sculpt_layers)==2
    assert m.sculpt_layers[0].name=='Protected Existing Layer' and abs(m.sculpt_layers[0].strength-.37)<1e-6
    assert tuple(tuple(v.co)for v in target.data.vertices)==base_coordinates
    bpy.ops.sculpt.multires_base_fingerprint(label='DT_AFTER',include_layer_payloads=True,include_surface=False)
    results['preview_error']=error
    results['source_unchanged']=hashlib.sha256(evaluated(source).tobytes()).hexdigest()==source_hash
    results['reference_unchanged']=hashlib.sha256(evaluated(reference).tobytes()).hexdigest()==reference_hash
    assert results['source_unchanged'] and results['reference_unchanged']

    # Undo/Redo the native import itself before any extra property operations.
    assert 'FINISHED' in bpy.ops.ed.undo()
    target=bpy.data.objects['DT_Target'];bpy.context.view_layer.objects.active=target
    assert len(dt.multires(target).sculpt_layers)==1
    assert maximum_error(evaluated(target),before)<3e-5
    assert 'FINISHED' in bpy.ops.ed.redo()
    target=bpy.data.objects['DT_Target'];bpy.context.view_layer.objects.active=target
    assert len(dt.multires(target).sculpt_layers)==2
    assert maximum_error(evaluated(target),expected)<3e-5
    m=dt.multires(target);m.sculpt_layers[1].strength=0.0
    bpy.context.view_layer.update()
    assert maximum_error(evaluated(target),before)<3e-5
    m.sculpt_layers[1].strength=1.0
    bpy.context.view_layer.update()

    # Stale preview must reject before adding any layer.
    preview=bpy.context.scene.detail_transfer_v01.preview
    preview.data.attributes[dt.ORIGIN].data[0].vector.z+=.25
    count=len(m.sculpt_layers)
    try:
        result=bpy.ops.sculpt.multires_detail_transfer_import_v01('EXEC_DEFAULT', True,preview_object=preview.name)
    except RuntimeError:
        result={'CANCELLED'}
    assert 'CANCELLED' in result and len(m.sculpt_layers)==count
    preview.hide_set(True)

    path=ROOT/'demo-detail-transfer-v01.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    bpy.ops.wm.open_mainfile(filepath=str(path))
    target=bpy.data.objects['DT_Target'];bpy.context.view_layer.objects.active=target
    assert len(dt.multires(target).sculpt_layers)==2
    assert maximum_error(evaluated(target),expected)<3e-5
    with bpy.context.temp_override(active_object=target, object=target):
        bpy.ops.sculpt.multires_base_fingerprint(label='DT_REOPEN',include_layer_payloads=True,include_surface=False)
    results.update(status='PASS',undo_redo=True,stale_preview_rejected=True,save_reopen=True)
    (ROOT/'risultati-test-blender.json').write_text(json.dumps(results,indent=2))
    print('[DETAIL_TRANSFER_TEST] '+json.dumps(results),flush=True)
except Exception:
    import traceback
    results['status']='FAIL';results['traceback']=traceback.format_exc()
    (ROOT/'risultati-test-blender.json').write_text(json.dumps(results,indent=2))
    raise
