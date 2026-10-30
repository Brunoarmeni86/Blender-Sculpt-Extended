from pathlib import Path
import bpy,bmesh,sys,importlib.util,json,traceback,math
from mathutils import Matrix,Vector
r=Path(__file__).parent
spec=importlib.util.spec_from_file_location('preview_transition',r/'symmetry_reference_preview_transition.py');m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m);m.register()
def state():
 ob=bpy.context.object
 if ob.mode=='EDIT':
  bm=bmesh.from_edit_mesh(ob.data);bm.verts.index_update();mask=bm.verts.layers.float['.sculpt_mask']
  return ([tuple(v.co) for v in bm.verts],[tuple(v.index for v in f.verts) for f in bm.faces],[v[mask] for v in bm.verts])
 me=ob.data
 return ([tuple(v.co) for v in me.vertices],[tuple(p.vertices) for p in me.polygons],[x.value for x in me.attributes['.sculpt_mask'].data])
def frozen():
 ob=bpy.context.object;own=ob.mode!='EDIT';bm=bmesh.new() if own else bmesh.from_edit_mesh(ob.data)
 if own:bm.from_mesh(ob.data)
 mask=bm.verts.layers.float['.sculpt_mask'];mark=bm.verts.layers.float['test_detail'];face_mark=bm.faces.layers.int['test_face']
 uv=bm.loops.layers.uv.get('UVMap')
 rows=[(tuple(v.co),v[mask],v[mark]) for v in bm.verts if v[mask]>=1-1e-6]
 rows.extend(('FACE',f[face_mark],tuple(tuple(loop[uv].uv) for loop in f.loops)) for f in bm.faces)
 if own:bm.free()
 return sorted(rows,key=repr)
def run():
 rows=[]
 try:
  for mode in ('SCULPT','EDIT'):
   for space in ('LOCAL','REFERENCE'):
    if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    me=bpy.data.meshes.new('TransitionTest');ob=bpy.data.objects.new('TransitionTest',me);bpy.context.collection.objects.link(ob);bpy.context.view_layer.objects.active=ob;ob.select_set(True)
    bm=bmesh.new();bmesh.ops.create_uvsphere(bm,u_segments=40,v_segments=24,radius=1)
    uv=bm.loops.layers.uv.new('UVMap');mask=bm.verts.layers.float.new('.sculpt_mask');mark=bm.verts.layers.float.new('test_detail');face_mark=bm.faces.layers.int.new('test_face')
    for v in bm.verts:
     v[mask]=1 if v.co.x>0 and v.co.z>.35 else 0;v[mark]=v.co.z*.123
     if v.co.x>0:v.co.x*=1+.1*math.sin(v.co.z*15)
    bmesh.ops.triangulate(bm,faces=[f for f in bm.faces if all(v.co.x<-.2 and v.co.z<-.3 for v in f.verts)])
    frame=Matrix.Translation((.2,-.15,.1))@Matrix.Rotation(.35,4,'Z') if space=='REFERENCE' else Matrix.Identity(4)
    for v in bm.verts:v.co=frame@v.co;v.select_set(True)
    for i,f in enumerate(bm.faces):
     f[face_mark]=i+1;f.select_set(True)
     for loop in f.loops:loop[uv].uv=(loop.vert.co.y*.3,loop.vert.co.z*.4)
    bm.to_mesh(me);bm.free();s=ob.sculpt_symref_preview;s.edit_direction='X';s.location=frame.translation+(frame.to_3x3()@Vector((.035,0,0)) if space=='REFERENCE' else Vector((0,0,0)));s.rotation=frame.to_euler('XYZ');s.edit_selected_only=False
    bpy.ops.object.mode_set(mode=mode);bpy.ops.ed.undo_push(message='Before transition')
    before=state();fixed=frozen();inverse=frame.inverted();target_cap={co for co in before[0] if (inverse@Vector(co)).x<-.1 and (inverse@Vector(co)).z>.7};result=bpy.ops.mesh.symmetrize_reference_shape_preview('EXEC_DEFAULT',True,space=space,method='SURFACE')
    assert result=={'FINISHED'},result
    after=state();print('CAP_MOVEMENT',[(before[0][i],after[0][i],(Vector(before[0][i])-Vector(after[0][i])).length) for i in range(len(before[0])) if before[0][i] in target_cap and before[0][i]!=after[0][i]][:6],flush=True);assert target_cap.issubset(set(after[0])),'Masked source was copied';assert before!=after;assert fixed==frozen(),'Protected geometry/attributes changed'
    for _ in range(3):
     bpy.ops.ed.undo();assert state()==before,'Undo mismatch'
     bpy.ops.ed.redo();assert state()==after,'Redo mismatch'
    rows.append(dict(mode=mode,space=space,status='PASS',protected_geometry_attributes_exact=True,undo_redo_cycles=3,vertices=len(after[0])))
  report=dict(status='PASS',cases=rows)
 except Exception:report=dict(status='ERROR',cases=rows,error=traceback.format_exc())
 (r/'SOURCE-MASK-SHAPE-GUI.json').write_text(json.dumps(report,indent=2));print(report,flush=True);bpy.ops.wm.quit_blender()
bpy.app.timers.register(run,first_interval=1)
