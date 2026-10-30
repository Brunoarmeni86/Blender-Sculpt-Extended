from pathlib import Path
import bpy,bmesh,sys,json,traceback
from mathutils import Matrix
r=Path(__file__).parent;sys.path.insert(0,str(r));from symmetry_transition_build import assemble
rows=[]
for offset in (0,.025,.1):
 for pattern in ('source_cap','both_cap','side_patch'):
  bm=bmesh.new();bmesh.ops.create_uvsphere(bm,u_segments=24,v_segments=16,radius=1);mask=bm.verts.layers.float.new('.sculpt_mask')
  for v in bm.verts:
   v[mask]=1 if ((v.co.x>0 and v.co.z>.3) if pattern=='source_cap' else v.co.z>.3 if pattern=='both_cap' else v.co.y>.45 and abs(v.co.z)<.5) else 0
   if v.co.x>0 and v.co.z<.2:v.co.x*=1.08
  bmesh.ops.triangulate(bm,faces=[f for f in bm.faces if f.calc_center_median().x<-.3 and f.calc_center_median().z<0])
  try:
   result,info=assemble(bm,Matrix.Translation((offset,0,0)),0,1,.25);result.free();rows.append(dict(offset=offset,pattern=pattern,status='PASS'))
  except Exception as ex:rows.append(dict(offset=offset,pattern=pattern,status='REJECTED',error=str(ex)))
  bm.free()
(r/'TRANSITION-MASK-MATRIX.json').write_text(json.dumps(rows,indent=2));print(rows,flush=True)
