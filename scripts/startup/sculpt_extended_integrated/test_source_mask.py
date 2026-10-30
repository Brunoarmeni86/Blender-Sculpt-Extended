from pathlib import Path
import bpy,bmesh,sys,json,traceback,math
from mathutils import Matrix
r=Path(__file__).parent;sys.path.insert(0,str(r));from symmetry_transition_build import assemble
from symmetry_topology_mask import reconstruct
from symmetry_shape_v2 import transfer_checked
rows=[]
try:
 for method in ('TOPOLOGY','SURFACE','RECONSTRUCT'):
  bm=bmesh.new();bmesh.ops.create_uvsphere(bm,u_segments=40,v_segments=24,radius=1);mask=bm.verts.layers.float.new('.sculpt_mask')
  for v in bm.verts:
   v[mask]=1 if v.co.x>0 and v.co.z>.25 else 0
   if v.co.x>0 and v.co.z<.1:v.co.x*=1.1
  # The target upper cap is deliberately unmasked. It must still be excluded
  # because the corresponding source geometry is masked.
  expected={tuple(v.co) for v in bm.verts if v.co.x<-.2 and v.co.z>.6}
  protected_source={tuple(v.co) for v in bm.verts if v[mask]>0}
  before={tuple(v.co) for v in bm.verts}
  if method!='TOPOLOGY':bmesh.ops.triangulate(bm,faces=[f for f in bm.faces if all(v.co.x<-.2 and v.co.z<-.3 for v in f.verts)])
  if method=='RECONSTRUCT':result,info=assemble(bm,Matrix.Identity(4),0,1,.25)
  else:
   transfer_checked(bm,Matrix.Identity(4),0,1,.1,False,method);result=bm
  actual={tuple(v.co) for v in result.verts}
  assert expected.issubset(actual),'Masked source copied into destination'
  assert protected_source.issubset(actual),'Masked source changed'
  assert before!=actual,'Nothing changed in the free region'
  rows.append(dict(method=method,status='PASS',source_excluded=True,target_counterpart_unchanged=True,free_region_changed=True))
  if result is not bm:result.free()
  bm.free()
 report=dict(status='PASS',cases=rows)
except Exception:report=dict(status='ERROR',cases=rows,error=traceback.format_exc())
(r/'SOURCE-MASK-TEST.json').write_text(json.dumps(report,indent=2));print(report,flush=True)
