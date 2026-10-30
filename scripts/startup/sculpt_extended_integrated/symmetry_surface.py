"""Continuous surface correspondence on two disk-like halves; caller mesh remains unchanged."""
import bpy,math,time,heapq,os
from array import array
from mathutils import Vector
from mathutils.bvhtree import BVHTree

class SurfaceError(ValueError):pass

def clipped_half(co,triangles,axis,sign):
    points=[];keys=[];lookup={};original={};faces=[]
    def vertex(i):
        key=('v',i)
        if key not in lookup:
            lookup[key]=len(points);points.append(co[i].copy());keys.append(key);original[i]=lookup[key]
        return lookup[key]
    def crossing(i,j):
        if co[i][axis]==0:return vertex(i)
        if co[j][axis]==0:return vertex(j)
        key=('e',min(i,j),max(i,j))
        if key not in lookup:
            p=co[i].lerp(co[j],co[i][axis]/(co[i][axis]-co[j][axis]));p[axis]=0
            lookup[key]=len(points);points.append(p);keys.append(key)
        return lookup[key]
    for tri in triangles:
        polygon=[]
        for k,i in enumerate(tri):
            j=tri[(k+1)%3];inside=co[i][axis]*sign>=0;other=co[j][axis]*sign>=0
            if inside:polygon.append(vertex(i))
            if inside!=other:polygon.append(crossing(i,j))
        polygon=[v for k,v in enumerate(polygon) if not k or v!=polygon[k-1]]
        if len(polygon)>1 and polygon[0]==polygon[-1]:polygon.pop()
        for k in range(1,len(polygon)-1):
            a,b,c=polygon[0],polygon[k],polygon[k+1]
            if (points[b]-points[a]).cross(points[c]-points[a]).length_squared>1e-28:faces.append((a,b,c))
    used=sorted({i for face in faces for i in face})
    remap={old:new for new,old in enumerate(used)}
    points=[points[i] for i in used];keys=[keys[i] for i in used]
    faces=[tuple(remap[i] for i in face) for face in faces]
    original={old:remap[i] for old,i in original.items() if i in remap}
    lookup={key:i for i,key in enumerate(keys)}
    edges={}
    for face in faces:
        for k,i in enumerate(face):
            j=face[(k+1)%3];key=(min(i,j),max(i,j));edges[key]=edges.get(key,0)+1
    if any(n>2 for n in edges.values()):raise SurfaceError('Taglio non manifold')
    boundary={}
    for (a,b),n in edges.items():
        if n==1:boundary.setdefault(a,[]).append(b);boundary.setdefault(b,[]).append(a)
    if not boundary or any(len(neighbours)!=2 for neighbours in boundary.values()):raise SurfaceError('Bordo del taglio non chiuso')
    if len(points)-len(edges)+len(faces)!=1:raise SurfaceError('La metà non è un disco topologico')
    start=min(boundary,key=lambda i:keys[i]);order=[start];previous=-1;current=start
    while True:
        next_vertex=next(v for v in boundary[current] if v!=previous)
        if next_vertex==start:break
        if next_vertex in order[-2:]:raise SurfaceError('Bordo ambiguo')
        order.append(next_vertex);previous,current=current,next_vertex
        if len(order)>len(boundary):raise SurfaceError('Bordo non semplice')
    if len(order)!=len(boundary):raise SurfaceError('Servirebbe gestire più contorni separati')
    if any(abs(points[i][axis])>1e-7 for i in boundary):raise SurfaceError('Bordi aperti lontani dal piano')
    return dict(points=points,keys=keys,lookup=lookup,original=original,faces=faces,edges=list(edges),boundary=order)

def parameterize(half,boundary_uv):
    mesh=bpy.data.meshes.new('__symref_param_probe')
    try:
        pins=[key in boundary_uv for key in half['keys']]
        mesh.from_pydata([(*boundary_uv[key],0) if key in boundary_uv else (0,0,0) for key in half['keys']],half['edges'],[])
        attribute=mesh.attributes.new('.symref_pin','BOOLEAN','POINT');attribute.data.foreach_set('value',pins)
        if bpy.ops.sculpt.reference_harmonic_solve_preview(mesh=mesh.name)!={'FINISHED'}:raise SurfaceError('Solver armonico non riuscito')
        values=array('f',[0])*(len(half['points'])*3);mesh.vertices.foreach_get('co',values)
        uv=[Vector(values[i:i+3]) for i in range(0,len(values),3)]
    finally:bpy.data.meshes.remove(mesh)
    areas=[]
    for a,b,c in half['faces']:
        p,q,t=uv[a],uv[b],uv[c];areas.append((q.x-p.x)*(t.y-p.y)-(q.y-p.y)*(t.x-p.x))
    positive=sum(a>0 for a in areas);negative=sum(a<0 for a in areas)
    if positive and negative:raise SurfaceError('Parametrizzazione con triangoli invertiti')
    if min(abs(a) for a in areas)<1e-18:raise SurfaceError('Parametrizzazione troppo compressa')
    return uv,min(abs(a) for a in areas)

def candidate(bm,axis=0,sign=1,correct_boundary=True,transition_width=.15):
    start=time.perf_counter();bm.verts.ensure_lookup_table();bm.verts.index_update();bm.normal_update()
    vertices=list(bm.verts);co=[v.co.copy() for v in vertices]
    size=max(max(p[k] for p in co)-min(p[k] for p in co) for k in range(3));epsilon=max(1e-10,size*5e-7)
    cut_co=[p.copy() for p in co]
    for p in cut_co:
        if abs(p[axis])<epsilon:p[axis]=0
    triangles=[[loop.vert.index for loop in tri] for tri in bm.calc_loop_triangles()]
    source=clipped_half(cut_co,triangles,axis,sign);target=clipped_half(cut_co,triangles,axis,-sign)
    source_keys={source['keys'][i] for i in source['boundary']};target_keys={target['keys'][i] for i in target['boundary']}
    if source_keys!=target_keys:raise SurfaceError('I due bordi del taglio non coincidono')
    order=source['boundary'];lengths=[(source['points'][order[(i+1)%len(order)]]-source['points'][v]).length for i,v in enumerate(order)]
    total=sum(lengths);walk=0;boundary_uv={}
    for i,v in enumerate(order):
        angle=2*math.pi*walk/total;boundary_uv[source['keys'][v]]=(math.cos(angle),math.sin(angle));walk+=lengths[i]
    source_uv,source_min=parameterize(source,boundary_uv);target_uv,target_min=parameterize(target,boundary_uv)
    tree=BVHTree.FromPolygons(source_uv,source['faces'],all_triangles=True)
    def sample(p):
        q,normal,index,distance=tree.find_nearest(p)
        if q is None or distance>1e-5:raise SurfaceError('Punto fuori dalla superficie parametrizzata')
        a,b,c=source['faces'][index];pa,pb,pc=source_uv[a],source_uv[b],source_uv[c]
        den=(pb.x-pa.x)*(pc.y-pa.y)-(pb.y-pa.y)*(pc.x-pa.x)
        wb=((q.x-pa.x)*(pc.y-pa.y)-(q.y-pa.y)*(pc.x-pa.x))/den
        wc=((pb.x-pa.x)*(q.y-pa.y)-(pb.y-pa.y)*(q.x-pa.x))/den;wa=1-wb-wc
        value=source['points'][a]*wa+source['points'][b]*wb+source['points'][c]*wc
        normal=(source['points'][b]-source['points'][a]).cross(source['points'][c]-source['points'][a]).normalized()
        value[axis]*=-1;normal[axis]*=-1
        source_values=[]
        for index in (a,b,c):
            key=source['keys'][index]
            if not mask:source_values.append(0)
            elif key[0]=='v':source_values.append(vertices[key[1]][mask])
            else:
                lo,hi=key[1:];fraction=cut_co[lo][axis]/(cut_co[lo][axis]-cut_co[hi][axis])
                source_values.append(vertices[lo][mask]*(1-fraction)+vertices[hi][mask]*fraction)
        return value,normal,max(0,min(1,max(source_values)))
    mask=bm.verts.layers.float.get('.sculpt_mask');result=[p.copy() for p in co];changed=0;max_movement=0;weights=[0.]*len(co)
    corrected=None
    if correct_boundary:
        samples=[sample(p) for p in target_uv];wanted=[item[0] for item in samples];boundary=set(target['boundary']);reverse={cut:original for original,cut in target['original'].items()}
        pins=[];values=[]
        for j,p in enumerate(target['points']):
            original=reverse.get(j)
            fixed=j in boundary or samples[j][2]>=1-1e-6 or (original is not None and (vertices[original].hide or (mask and vertices[original][mask]>=1)))
            pins.append(bool(fixed));values.append(p-wanted[j] if fixed else Vector((0,0,0)))
        mesh=bpy.data.meshes.new('__symref_param_field')
        try:
            mesh.from_pydata(values,target['edges'],[])
            attr=mesh.attributes.new('.symref_pin','BOOLEAN','POINT');attr.data.foreach_set('value',pins)
            if bpy.ops.sculpt.reference_harmonic_solve_preview(mesh=mesh.name,vector_field=True)!={'FINISHED'}:raise SurfaceError('Adattamento alla maschera non riuscito')
            values=array('f',[0])*(len(target['points'])*3);mesh.vertices.foreach_get('co',values)
            # Fade only the correction imposed by the protected patch. The reflected
            # source stays exact away from the collar, rather than inheriting a
            # harmonic displacement throughout the whole destination half.
            radius=max(size*transition_width,epsilon*10)
            distance=[float('inf')]*len(pins);queue=[]
            neighbours=[[] for _ in pins]
            for a,b in target['edges']:
                length=(target['points'][a]-target['points'][b]).length
                neighbours[a].append((b,length));neighbours[b].append((a,length))
            for j,fixed in enumerate(pins):
                if fixed and (target['points'][j]-wanted[j]).length>epsilon:
                    distance[j]=0.;heapq.heappush(queue,(0.,j))
            while queue:
                d,j=heapq.heappop(queue)
                if d!=distance[j] or d>=radius:continue
                for k,length in neighbours[j]:
                    nd=d+length
                    if nd<distance[k] and nd<radius:
                        distance[k]=nd;heapq.heappush(queue,(nd,k))
            corrected=[]
            for j,point in enumerate(wanted):
                t=min(1.,distance[j]/radius)
                taper=1.-t*t*t*(t*(6*t-15)+10)
                corrected.append(point+Vector(values[3*j:3*j+3])*taper)
        finally:bpy.data.meshes.remove(mesh)
    for i,p in enumerate(co):
        if p[axis]*sign>=0 or vertices[i].hide:continue
        source_weight=sample(target_uv[target['original'][i]])[2]
        weight=1-max(0,min(1,max(vertices[i][mask] if mask else 0,source_weight)))
        if weight==0:continue
        if i not in target['original']:raise SurfaceError('Vertice destinazione senza coordinate')
        if corrected is None:desired=sample(target_uv[target['original'][i]])[0]
        else:desired=corrected[target['original'][i]]
        result[i]=p.lerp(desired,weight);weights[i]=weight
        movement=(result[i]-p).length;max_movement=max(max_movement,movement);changed+=movement>1e-8
    info=dict(changed=changed,max_movement=max_movement,source_uv_min_area=source_min,target_uv_min_area=target_min,seconds=time.perf_counter()-start)
    return result,info
