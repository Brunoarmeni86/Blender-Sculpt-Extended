"""Build and validate a complete candidate, preserving the input until success."""
import bmesh,importlib.util
from pathlib import Path
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

def attributes(destination,sources,weights,destination_layers,source_layers):
 nearest=max(range(len(weights)),key=weights.__getitem__)
 for kind in ('float','int','float_vector','color','float_color','string','deform','uv'):
  src=getattr(source_layers,kind,None);dst=getattr(destination_layers,kind,None)
  if src is None or dst is None:continue
  for name,layer in src.items():
   target=dst.get(name)
   if target is None:continue
   values=[element[layer] for element in sources]
   if kind=='float':destination[target]=sum(float(v)*w for v,w in zip(values,weights))
   elif kind in {'float_vector','color','float_color'}:
    destination[target]=tuple(sum(v[k]*w for v,w in zip(values,weights)) for k in range(len(values[0])))
   elif kind=='deform':
    result=destination[target]
    for key in list(result):del result[key]
    for key in set().union(*(v.keys() for v in values)):result[key]=sum(v.get(key,0)*w for v,w in zip(values,weights))
   elif kind=='uv':
    destination[target].uv=values[nearest].uv
   else:destination[target]=values[nearest]

def assemble(bm,transform,axis,sign,width):
 spec=importlib.util.spec_from_file_location('transition_uv',Path(__file__).with_name('symmetry_transition_uv.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
 if any(f.hide for f in bm.faces):raise ValueError('Mostra tutta la mesh prima del raccordo')
 if any(not e.is_manifold for e in bm.edges):raise ValueError('Il raccordo richiede una superficie chiusa e manifold')
 data=module.transition(bm,transform,axis,sign,width);inverse=transform.inverted()
 bm.verts.ensure_lookup_table();bm.faces.ensure_lookup_table();original=list(bm.verts)
 coords=[transform@v.co for v in original];triangles=list(bm.calc_loop_triangles());ids=[[l.vert.index for l in t] for t in triangles]
 tree=BVHTree.FromPolygons(coords,ids,all_triangles=True)
 result=bm.copy();donors=list(result.verts);donorfaces=list(result.faces)
 vertices=[];origins=[];original_map={};fragment_faces=[]
 def create(point,query,fixed=None):
  vertex=result.verts.new(inverse@point)
  if fixed is not None:
   attributes(vertex,[original[fixed]],[1],result.verts.layers,bm.verts.layers);vertex.co=original[fixed].co;face=None
  else:
   q,n,j,d=tree.find_nearest(query);face=triangles[j][0].face
   a,b,c=[donors[i] for i in ids[j]];pa,pb,pc=[coords[i] for i in ids[j]]
   u=pb-pa;v=pc-pa;w=q-pa;aa=u.dot(u);bb=u.dot(v);cc=v.dot(v);dd=w.dot(u);ee=w.dot(v);den=aa*cc-bb*bb
   if abs(den)<1e-20:raise ValueError('Triangolo degenere nella superficie di riferimento')
   wb=max(0,min(1,(cc*dd-bb*ee)/den));wc=max(0,min(1,(aa*ee-bb*dd)/den));wa=max(0,1-wb-wc);total=wa+wb+wc;wa/=total;wb/=total;wc/=total
   attributes(vertex,[original[i] for i in ids[j]],[wa,wb,wc],result.verts.layers,bm.verts.layers)
   vertex.co=inverse@point
  return vertex,face
 try:
  for i,p in enumerate(data['points']):
   query=data['base'][i].copy()
   if data['kinds'][i] in {'MIRROR','SEAM'}:query=p.copy();query[axis]*=-1
   v,f=create(p,query,data['fixed_vertices'].get(i));vertices.append(v);origins.append(f)
   if i in data['fixed_vertices']:original_map[data['fixed_vertices'][i]]=v
  for i,face in enumerate(data['faces']):
   new=result.faces.new([vertices[j] for j in face])
   fixed_start=data['mirror_faces'];fixed_end=fixed_start+data['fixed_faces']
   if fixed_start<=i<fixed_end:
    old=bm.faces[data['fixed_face_ids'][i-fixed_start]];
    if old.index in data['crossing_faces']:fragment_faces.append(new)
    attributes(new,[old],[1],result.faces.layers,bm.faces.layers);new.material_index=old.material_index;new.smooth=old.smooth
    for loop,source in zip(new.loops,old.loops):attributes(loop,[source],[1],result.loops.layers,bm.loops.layers)
   else:
    old=next((origins[j] for j in face if origins[j] is not None),None)
    if old is None:
     center=sum((data['base'][j] for j in face),Vector())/len(face)
     nearest=tree.find_nearest(center)
     if nearest[2] is None:raise ValueError('Faccia di raccordo senza superficie originale')
     old=triangles[nearest[2]][0].face
    attributes(new,[old],[1],result.faces.layers,bm.faces.layers);new.material_index=old.material_index;new.smooth=old.smooth
  # Keep the source geometry; use the original polygons wherever no cut is needed.
  lookup=KDTree(len(vertices))
  for i,v in enumerate(vertices):lookup.insert(transform@v.co,i)
  lookup.balance();source_vertices=[]
  for key,p in zip(data['source']['keys'],data['source']['points']):
   hit=lookup.find(p)
   if abs(p[axis])<1e-6 and hit[2]<1e-6:vertex=vertices[hit[1]]
   else:vertex,_=create(p,p,key[1] if key[0]=='v' else None)
   source_vertices.append(vertex)
   if key[0]=='v':original_map[key[1]]=vertex
  source_index=data['source']['original'];complete={f for f in bm.faces if all(coords[v.index][axis]*sign>=-1e-6 for v in f.verts)}
  for old in complete:
   new=result.faces.new([source_vertices[source_index[v.index]] for v in old.verts]);attributes(new,[old],[1],result.faces.layers,bm.faces.layers);new.material_index=old.material_index;new.smooth=old.smooth
   for loop,source in zip(new.loops,old.loops):attributes(loop,[source],[1],result.loops.layers,bm.loops.layers)
  for face in data['source']['faces']:
   keys=[data['source']['keys'][i] for i in face]
   owner_candidates=None
   for key in keys:
    for i in (key[1:] if key[0]=='e' else (key[1],)):
     linked={f.index for f in original[i].link_faces};owner_candidates=linked if owner_candidates is None else owner_candidates.intersection(linked)
   if owner_candidates and owner_candidates.intersection(data['crossing_faces']):continue
   if all(k[0]=='v' for k in keys):
    owners=set(original[keys[0][1]].link_faces)
    for key in keys[1:]:owners.intersection_update(original[key[1]].link_faces)
    if owners.intersection(complete):continue
   new=result.faces.new([source_vertices[i] for i in face])
   if owner_candidates:
    old=bm.faces[next(iter(owner_candidates))];attributes(new,[old],[1],result.faces.layers,bm.faces.layers);new.material_index=old.material_index;new.smooth=old.smooth
  for face in fragment_faces:result.faces.remove(face)
  for index in data['crossing_faces']:
   old=bm.faces[index]
   if any(v.index not in original_map for v in old.verts):raise ValueError('Vertice mancante nel raccordo sul piano')
   new=result.faces.new([original_map[v.index] for v in old.verts]);attributes(new,[old],[1],result.faces.layers,bm.faces.layers);new.material_index=old.material_index;new.smooth=old.smooth
   for loop,source in zip(new.loops,old.loops):attributes(loop,[source],[1],result.loops.layers,bm.loops.layers)
  bmesh.ops.delete(result,geom=donors,context='VERTS')
  for edge in list(result.edges):
   if not edge.link_faces:result.edges.remove(edge)
  for vertex in list(result.verts):
   if not vertex.link_edges:result.verts.remove(vertex)
  result.normal_update();result.faces.index_update();result.verts.index_update()
  bad=[e for e in result.edges if not e.is_manifold]
  if bad:
   import json
   details={'edges':len(bad),'boundary':sum(e.is_boundary for e in bad),'samples':[[tuple(v.co) for v in e.verts] for e in bad[:12]]}
   (Path(__file__).parent/'TRANSITION-BOUNDARY-DIAGNOSTIC.json').write_text(json.dumps(details,indent=2))
   raise ValueError(f'Il raccordo non è chiuso ({len(bad)} bordi): nessuna modifica')
  if any(e.link_loops[0].vert==e.link_loops[1].vert for e in result.edges):raise ValueError('Orientamento del raccordo non coerente')
  result.faces.ensure_lookup_table();bvh=BVHTree.FromBMesh(result,epsilon=1e-12)
  conflicts=[(a,b) for a,b in bvh.overlap(bvh) if a<b and not set(result.faces[a].verts).intersection(result.faces[b].verts)]
  if conflicts:raise ValueError(f'Raccordo con {len(conflicts)} intersezioni: nessuna modifica')
  fixed_original={tuple(sorted(tuple(v.co) for v in bm.faces[i].verts)) for i in data['fixed_face_ids']}
  final={tuple(sorted(tuple(v.co) for v in f.verts)) for f in result.faces}
  if not fixed_original.issubset(final):raise ValueError('Verifica della zona protetta fallita')
  return result,dict(fixed_faces=data['fixed_faces'],transition_faces=data['band_faces'],intersections=0)
 except Exception:result.free();raise
