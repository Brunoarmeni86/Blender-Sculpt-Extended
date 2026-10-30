"""Prototype: build a transition in a common UV domain, never by nearest 3D welding."""
import bpy,bmesh,math,importlib.util
from pathlib import Path
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import delaunay_2d_cdt

def load_surface():
 spec=importlib.util.spec_from_file_location('surface_transition',Path(__file__).with_name('symmetry_surface.py'))
 m=importlib.util.module_from_spec(spec)
 spec.loader.exec_module(m);return m

def transition(bm,transform,axis,sign,width):
 surface=load_surface();bm.verts.index_update();bm.faces.index_update()
 vs=list(bm.verts);co=[transform@v.co for v in vs];mask=bm.verts.layers.float.get('.sculpt_mask')
 if not mask:raise ValueError('Nessuna maschera')
 for p in co:
  if abs(p[axis])<1e-6:p[axis]=0
 # Keep a complete strip of original faces crossing the plane. Its target-side
 # fragments constrain the solver, but the assembler restores the whole polygons.
 crossing={f.index for f in bm.faces if min(co[v.index][axis]*sign for v in f.verts)<-1e-8 and max(co[v.index][axis]*sign for v in f.verts)>1e-8}
 protected=[f for f in bm.faces if (any(v[mask]>1e-6 for v in f.verts) or f.index in crossing or any(abs(co[v.index][axis])<1e-8 for v in f.verts)) and any(co[v.index][axis]*sign<0 for v in f.verts)]
 if not protected:raise ValueError('Nessuna faccia mascherata nella destinazione da raccordare')
 triangles=[[l.vert.index for l in t] for t in bm.calc_loop_triangles()]
 src=surface.clipped_half(co,triangles,axis,sign);dst=surface.clipped_half(co,triangles,axis,-sign)
 order=src['boundary'];lengths=[(src['points'][order[(k+1)%len(order)]]-src['points'][v]).length for k,v in enumerate(order)];total=sum(lengths);walk=0;uvboundary={}
 for k,v in enumerate(order):
  angle=2*math.pi*walk/total;uvboundary[src['keys'][v]]=(math.cos(angle),math.sin(angle));walk+=lengths[k]
 su,_=surface.parameterize(src,uvboundary);tu,_=surface.parameterize(dst,uvboundary)
 # Align the two parameterizations by geometry when that produces an injective
 # map. Independent harmonic solves can otherwise twist corresponding detail.
 source_geometry=BVHTree.FromPolygons(src['points'],src['faces'],all_triangles=True)
 aligned=[]
 for i,p in enumerate(dst['points']):
  key=dst['keys'][i]
  if key in uvboundary:aligned.append(Vector((*uvboundary[key],0)));continue
  reflected_point=p.copy();reflected_point[axis]*=-1
  q,normal,j,d=source_geometry.find_nearest(reflected_point)
  a,b,c=src['faces'][j];pa,pb,pc=src['points'][a],src['points'][b],src['points'][c]
  v0=pb-pa;v1=pc-pa;v2=q-pa;aa=v0.dot(v0);bb=v0.dot(v1);cc=v1.dot(v1);dd=v2.dot(v0);ee=v2.dot(v1);den=aa*cc-bb*bb
  wb=(cc*dd-bb*ee)/den;wc=(aa*ee-bb*dd)/den
  aligned.append(su[a]*(1-wb-wc)+su[b]*wb+su[c]*wc)
 areas=[(aligned[b].x-aligned[a].x)*(aligned[c].y-aligned[a].y)-(aligned[b].y-aligned[a].y)*(aligned[c].x-aligned[a].x) for a,b,c in dst['faces']]
 aligned_ok=min(areas)>1e-12 or max(areas)<-1e-12
 if aligned_ok:tu=aligned
 print('PARAMETER_ALIGNMENT',aligned_ok,flush=True)
 # A masked source is excluded from copying: freeze its corresponding target
 # region too, instead of reproducing the masked shape on the opposite side.
 source_uv_tree=BVHTree.FromPolygons(su,src['faces'],all_triangles=True)
 def source_mask_at(p):
  q,n,j,d=source_uv_tree.find_nearest(p)
  if q is None or d>1e-5:raise ValueError('Corrispondenza maschera fuori dominio')
  a,b,c=src['faces'][j];pa,pb,pc=su[a],su[b],su[c]
  den=(pb.x-pa.x)*(pc.y-pa.y)-(pb.y-pa.y)*(pc.x-pa.x)
  wb=((q.x-pa.x)*(pc.y-pa.y)-(q.y-pa.y)*(pc.x-pa.x))/den;wc=((pb.x-pa.x)*(q.y-pa.y)-(pb.y-pa.y)*(q.x-pa.x))/den
  weights=(1-wb-wc,wb,wc);values=[]
  for i in (a,b,c):
   key=src['keys'][i]
   if key[0]=='v':value=vs[key[1]][mask]
   else:
    lo,hi=key[1:];fraction=co[lo][axis]/(co[lo][axis]-co[hi][axis]);value=vs[lo][mask]*(1-fraction)+vs[hi][mask]*fraction
   values.append(value)
  return max(0,min(1,sum(w*v for w,v in zip(weights,values))))
 mapped_mask=[source_mask_at(p) for p in tu]
 protected_ids={f.index for f in protected}
 for face in bm.faces:
  if face.index in protected_ids:continue
  indices=[dst['original'].get(v.index) for v in face.verts]
  if any(co[v.index][axis]*sign<0 for v in face.verts) and any(i is not None and mapped_mask[i]>1e-6 for i in indices):protected.append(face);protected_ids.add(face.index)
 protected_polys=[];protected_owners=[];protected_points=[]
 target_triangle_owners=[];triangles_by_owner={}
 for face in dst['faces']:
  original_ids=set()
  for i in face:
   key=dst['keys'][i];original_ids.update(key[1:] if key[0]=='e' else (key[1],))
  possible=None
  for i in original_ids:
   linked={f.index for f in vs[i].link_faces};possible=linked if possible is None else possible.intersection(linked)
  if not possible:raise ValueError('Frammento del piano senza faccia originale')
  owner=next(iter(possible));target_triangle_owners.append(owner);triangles_by_owner.setdefault(owner,[]).append(face)
 for f in protected:
  if f.index not in crossing:
   protected_polys.append([dst['original'][v.index] for v in f.verts]);protected_owners.append(f.index)
  else:
   for face in triangles_by_owner.get(f.index,[]):protected_polys.append(list(face));protected_owners.append(f.index)
 patch=BVHTree.FromPolygons(tu,protected_polys)
 # Select complete source triangles outside a UV collar around the fixed patch.
 retained=[]
 for face in src['faces']:
  probes=[su[i] for i in face]+[sum((su[i] for i in face),Vector())/3]
  if all(patch.find_nearest(p)[3]>width for p in probes):retained.append(face)
 if not retained:raise ValueError('Fascia troppo ampia: nessuna superficie speculare rimasta')
 # All domain vertices have explicit 3D origins; new CDT vertices are checked later.
 points=[];world=[];lookup={};kinds=[]
 def add(p,position,kind):
  key=(round(p.x,10),round(p.y,10))
  if key in lookup:
   i=lookup[key]
   if (world[i]-position).length>1e-5:raise ValueError('Bordi coincidenti con geometria incompatibile')
   return i
  i=len(points);lookup[key]=i;points.append(Vector((p.x,p.y)));world.append(position.copy());kinds.append(kind);return i
 def reflected(p):
  p=p.copy();p[axis]*=-1;return p
 constraints=[];types=[];fixed_vertices={}
 for face in retained:
  constraints.append([add(su[i],reflected(src['points'][i]),'MIRROR') for i in reversed(face)]);types.append('MIRROR')
 for owner,poly in zip(protected_owners,protected_polys):
  face_indices=[add(tu[i],dst['points'][i],'FIXED') for i in poly]
  for j,i in zip(face_indices,poly):
   key=dst['keys'][i]
   if key[0]=='v':fixed_vertices[j]=key[1]
  constraints.append(face_indices);types.append('FIXED')
 boundary=[add(su[i],reflected(src['points'][i]),'SEAM') for i in src['boundary']]
 edges=[(boundary[k],boundary[(k+1)%len(boundary)]) for k in range(len(boundary))]
 # A common parameterization provides both original and mirrored geometry everywhere.
 source_tree=BVHTree.FromPolygons(su,src['faces'],all_triangles=True);target_tree=BVHTree.FromPolygons(tu,dst['faces'],all_triangles=True)
 def sample(tree,half,coords,p):
  q,n,j,d=tree.find_nearest(Vector((p.x,p.y,0)))
  if q is None or d>1e-5:raise ValueError('Campione fuori dominio')
  a,b,c=half['faces'][j];pa,pb,pc=coords[a],coords[b],coords[c]
  den=(pb.x-pa.x)*(pc.y-pa.y)-(pb.y-pa.y)*(pc.x-pa.x)
  wb=((q.x-pa.x)*(pc.y-pa.y)-(q.y-pa.y)*(pc.x-pa.x))/den;wc=((pb.x-pa.x)*(q.y-pa.y)-(pb.y-pa.y)*(q.x-pa.x))/den
  return half['points'][a]*(1-wb-wc)+half['points'][b]*wb+half['points'][c]*wc
 # Retain detail samples from both surfaces inside the transition, rather than
 # spanning the collar with a few large flat triangles.
 retained_tree=BVHTree.FromPolygons(su,retained,all_triangles=True)
 from statistics import median
 spacing=max(1e-5,median((su[a]-su[b]).length for a,b in src['edges'])*.35)
 buckets={}
 def cell(p):return (math.floor(p.x/spacing),math.floor(p.y/spacing))
 for q in points:buckets.setdefault(cell(q),[]).append(q)
 for p in list(tu)+list(su):
  distance=patch.find_nearest(p)[3]
  if distance<=1e-8 or distance>=width or retained_tree.find_nearest(p)[3]<1e-8:continue
  x,y=cell(p)
  if any((p.xy-q).length<spacing for dx in (-1,0,1) for dy in (-1,0,1) for q in buckets.get((x+dx,y+dy),[])):continue
  buckets.setdefault((x,y),[]).append(p.xy.copy())
  t=distance/width;t=t*t*(3-2*t)
  position=sample(target_tree,dst,tu,p).lerp(reflected(sample(source_tree,src,su,p)),t)
  add(p,position,'BAND')
 cdt_faces=[]
 for face in constraints:
  area=sum(points[face[k]].x*points[face[(k+1)%len(face)]].y-points[face[(k+1)%len(face)]].x*points[face[k]].y for k in range(len(face)))
  cdt_faces.append(face if area>0 else face[::-1])
 result=delaunay_2d_cdt(points,edges,cdt_faces,0,1e-9,True)
 uv,edgesout,facesout,origverts,origedges,origfaces=result
 print('CDT',len(uv),len(facesout),sum(not x for x in origfaces),flush=True)
 output=[]
 for i,p in enumerate(uv):
  if origverts[i]:output.append(world[origverts[i][0]].copy());continue
  # New points follow the original detailed surface near the mask, mirrored outside.
  distance=patch.find_nearest(Vector((p.x,p.y,0)))[3];t=min(1,max(0,distance/width));t=t*t*(3-2*t)
  old=sample(target_tree,dst,tu,p);new=reflected(sample(source_tree,src,su,p));output.append(old.lerp(new,t))
 # Preserve the original detailed target as a baseline; solve a smooth displacement
 # field with fixed mask and exact mirrored outer constraints.
 from array import array
 base=[sample(target_tree,dst,tu,p) for p in uv]
 pins=[bool(indices and any(kinds[j]!='BAND' for j in indices)) for indices in origverts]
 values=[output[i]-base[i] if pins[i] else Vector((0,0,0)) for i in range(len(uv))]
 temp=bpy.data.meshes.new('__symref_param_transition')
 try:
  temp.from_pydata(values,edgesout,[])
  attr=temp.attributes.new('.symref_pin','BOOLEAN','POINT');attr.data.foreach_set('value',pins)
  if bpy.ops.sculpt.reference_harmonic_solve_preview(mesh=temp.name,vector_field=True)!={'FINISHED'}:raise ValueError('Adattamento della fascia non riuscito')
  data=array('f',[0])*(3*len(uv));temp.vertices.foreach_get('co',data)
  output=[output[i] if pins[i] else base[i]+Vector(data[3*i:3*i+3]) for i in range(len(uv))]
 finally:bpy.data.meshes.remove(temp)
 # Preserve protected polygons and source triangles verbatim, triangulate only the collar.
 reverse={old:i for i,indices in enumerate(origverts) for old in indices}
 final_faces=[[reverse[i] for i in face] for face in constraints]
 target_face=constraints[-1];target_area=sum(points[target_face[k]].x*points[target_face[(k+1)%len(target_face)]].y-points[target_face[(k+1)%len(target_face)]].x*points[target_face[k]].y for k in range(len(target_face)))
 band=[face if target_area>0 else face[::-1] for face,owners in zip(facesout,origfaces) if not owners]
 final_faces+=band
 return dict(points=output,faces=final_faces,uv=uv,band_faces=len(band),fixed_faces=len(protected_polys),mirror_faces=len(retained),source=src,source_uv=su,base=base,fixed_vertices={reverse[j]:i for j,i in fixed_vertices.items()},fixed_face_ids=protected_owners,crossing_faces=sorted(crossing),kinds=[kinds[indices[0]] if indices else 'BAND' for indices in origverts])
