"""Topology-preserving, masked shape transfer in the preview reference frame."""
import bpy
import bmesh
from mathutils.kdtree import KDTree
from mathutils import Matrix
from bpy.props import FloatProperty, EnumProperty
from collections import deque
import math
from statistics import median
from mathutils.bvhtree import BVHTree
import importlib.util, sys
from pathlib import Path


class CorrespondenceError(ValueError):
    pass


def topology_pairs(bm, coordinates, tolerance):
    """Seed in unchanged regions, then propagate reversed face connectivity.

    Geometric matching alone must never publish a subset on a deformed surface.
    All correspondence is validated before any coordinate is written.
    """
    bm.verts.ensure_lookup_table();bm.verts.index_update()
    bm.faces.ensure_lookup_table();bm.faces.index_update()
    vertices=list(bm.verts);faces=list(bm.faces)
    if not vertices:return []
    if not all(all(math.isfinite(c) for c in p) for p in coordinates):
        raise CorrespondenceError('Coordinate non valide: nessuna modifica applicata')
    tree=KDTree(len(vertices))
    for i,p in enumerate(coordinates):tree.insert(p,i)
    tree.balance()
    # The caller supplies coordinates already expressed in a frame whose X is the mirror axis.
    reflected=[p.copy() for p in coordinates]
    for p in reflected:p.x=-p.x
    extent=max(max(p[k] for p in coordinates)-min(p[k] for p in coordinates) for k in range(3))
    strict=min(tolerance,max(extent*1e-6,1e-8))
    tie=max(extent*1e-9,1e-12)
    nearest=[]
    for p in reflected:
        hits=tree.find_n(p,2)
        nearest.append(hits[0] if hits and (len(hits)<2 or hits[1][2]-hits[0][2]>tie) else None)
    candidates=[]
    for i,hit in enumerate(nearest):
        if hit is None:continue
        _,j,distance=hit
        if distance<=tolerance and nearest[j] is not None and nearest[j][1]==i and i<=j:
            candidates.append((i,j,distance))
    mirror=[-1]*len(vertices);face_map=[-1]*len(faces)
    queue=deque();queued=bytearray(len(faces))

    def enqueue(vertex):
        for face in vertex.link_faces:
            if face_map[face.index]<0 and not queued[face.index]:
                queued[face.index]=1;queue.append(face)

    def assign(i,j):
        if mirror[i] not in (-1,j) or mirror[j] not in (-1,i):
            raise CorrespondenceError('Corrispondenza topologica ambigua: nessuna modifica applicata')
        if mirror[i]==j:return
        mirror[i]=j;mirror[j]=i;enqueue(vertices[i]);enqueue(vertices[j])

    def propagate():
        while queue:
            face=queue.popleft();queued[face.index]=0
            if face_map[face.index]>=0:continue
            for start in face.loops:
                a=mirror[start.vert.index];b=mirror[start.link_loop_next.vert.index]
                if a<0 or b<0:continue
                edge=bm.edges.get((vertices[a],vertices[b]))
                if edge is None:
                    raise CorrespondenceError('La topologia non ha una corrispondenza speculare completa')
                possibilities=[]
                for target in edge.link_loops:
                    if target.vert.index!=b or target.link_loop_next.vert.index!=a:continue
                    target=target.link_loop_next
                    other=target.face
                    if len(other.verts)!=len(face.verts) or face_map[other.index] not in (-1,face.index):continue
                    proposal={};source_loop=start;target_loop=target;valid=True
                    for _ in range(len(face.verts)):
                        i=source_loop.vert.index;j=target_loop.vert.index
                        if mirror[i] not in (-1,j) or mirror[j] not in (-1,i) or proposal.get(i,j)!=j or proposal.get(j,i)!=i:
                            valid=False;break
                        proposal[i]=j;proposal[j]=i
                        source_loop=source_loop.link_loop_next;target_loop=target_loop.link_loop_prev
                    if valid:possibilities.append((other,proposal))
                if not possibilities:
                    raise CorrespondenceError('Corrispondenza incompatibile fra le facce: nessuna modifica applicata')
                if len(possibilities)!=1:continue
                other,proposal=possibilities[0]
                face_map[face.index]=other.index;face_map[other.index]=face.index
                for i,j in proposal.items():assign(i,j)
                break

    def seed_face(face,limit):
        # One coherent face establishes its connected component. Isolated nearest points
        # in folded/deformed areas must not be installed as hard constraints beforehand.
        first=face.loops[0];hits=[]
        for loop in face.loops:
            i=loop.vert.index;hit=nearest[i]
            if hit is None or hit[2]>limit:return False
            j=hit[1]
            if nearest[j] is None or nearest[j][1]!=i or mirror[i] not in (-1,j):return False
            hits.append(j)
        edge=bm.edges.get((vertices[hits[0]],vertices[hits[1]]))
        if edge is None:return False
        options=[]
        for loop in edge.link_loops:
            if loop.vert.index!=hits[1] or loop.link_loop_next.vert.index!=hits[0]:continue
            target=loop.link_loop_next;other=target.face
            if len(other.verts)!=len(face.verts) or face_map[other.index] not in (-1,face.index):continue
            walk=target;matches=True
            for j in hits:
                if walk.vert.index!=j:matches=False;break
                walk=walk.link_loop_prev
            if matches:options.append(other)
        if len(options)!=1:return False
        other=options[0];face_map[face.index]=other.index;face_map[other.index]=face.index
        for loop,j in zip(face.loops,hits):assign(loop.vert.index,j)
        propagate();return True

    for limit in (strict,tolerance):
        for face in faces:
            if face_map[face.index]<0:seed_face(face,limit)
    # Loose/wire vertices have no face seed. Keep their mapping reciprocal and verify edges below.
    for i,j,distance in candidates:
        if mirror[i]<0 and mirror[j]<0 and not vertices[i].link_faces and not vertices[j].link_faces:
            assign(i,j)
    if any(i<0 for i in mirror) or any(i<0 for i in face_map):
        raise CorrespondenceError('Impossibile ricostruire tutte le coppie dalla topologia; nessuna modifica applicata')
    for edge in bm.edges:
        if bm.edges.get((vertices[mirror[edge.verts[0].index]],vertices[mirror[edge.verts[1].index]])) is None:
            raise CorrespondenceError('Corrispondenza degli spigoli incompatibile: nessuna modifica applicata')
    votes=[mirror[i]==j for i,j,distance in candidates if distance<=strict]
    if votes and sum(votes)<len(votes)/2:
        raise CorrespondenceError('La corrispondenza topologica non è coerente con il piano scelto')
    return mirror


def topology_sides(bm, mirror, coordinates, axis):
    """Keep the two topological halves stable when a sculpt crosses the plane."""
    vertices=list(bm.verts);component=[-1]*len(vertices);groups=[]
    for root in range(len(vertices)):
        if mirror[root]==root or component[root]>=0:continue
        index=len(groups);group=[];stack=[root];component[root]=index
        while stack:
            i=stack.pop();group.append(i)
            for edge in vertices[i].link_edges:
                j=edge.other_vert(vertices[i]).index
                if mirror[j]==j or j==mirror[i]:continue
                if component[j]<0:component[j]=index;stack.append(j)
        groups.append(group)
    sides=[0]*len(vertices)
    for k,group in enumerate(groups):
        if sides[group[0]]:continue
        other=component[mirror[group[0]]]
        if other<0 or other==k or any(component[mirror[i]]!=other for i in group):
            raise CorrespondenceError('La topologia non permette di distinguere due metà speculari')
        a=median(coordinates[i][axis] for i in group)
        b=median(coordinates[i][axis] for i in groups[other])
        if abs(a-b)<1e-8:raise CorrespondenceError('Lato sorgente ambiguo rispetto al piano scelto')
        sign=1 if a>b else -1
        for i in group:sides[i]=sign
        for i in groups[other]:sides[i]=-sign
    return sides


def transfer(bm, transform, axis, source_sign, tolerance, selected_only):
    vertices=list(bm.verts)
    coordinates=[transform @ v.co for v in vertices]
    # Permute the chosen axis to X for the connectivity solver; no coordinates are modified.
    ordered=[p.copy() for p in coordinates]
    if axis!=0:
        for p in ordered:p[0],p[axis]=p[axis],p[0]
    mirror=topology_pairs(bm,ordered,tolerance)
    sides=topology_sides(bm,mirror,coordinates,axis)
    mask=bm.verts.layers.float.get('.sculpt_mask')
    inverse=transform.inverted();changes=[];skipped=0
    for i,v in enumerate(vertices):
        j=mirror[i]
        if j!=i and sides[i]*source_sign>=0:continue
        v=vertices[i]
        if v.hide or (selected_only and not v.select):continue
        weight=1-max(0,min(1,max(v[mask],vertices[j][mask]))) if mask else 1
        if weight==0:continue
        if j!=i and vertices[j].hide:
            raise CorrespondenceError('Sorgente nascosta: nessuna modifica applicata')
        point=coordinates[j].copy();point[axis]=0 if j==i else -point[axis]
        target=inverse@point
        value=v.co.lerp(target,weight)
        if (value-v.co).length_squared>0:changes.append((v,value))
    for v,co in changes:v.co=co
    return len(changes),skipped


def transfer_checked(bm, transform, axis, source_sign, tolerance, selected_only, method, transition_width=.15):
    bm.verts.ensure_lookup_table();bm.verts.index_update();bm.faces.ensure_lookup_table();bm.faces.index_update()
    vertices=list(bm.verts)
    before=[v.co.copy() for v in vertices]
    working=bm.copy()
    try:
        for original,copy in zip(before,working.verts):
            if original!=copy.co:raise CorrespondenceError('Ordine dei vertici non coerente')
        chosen='topologia'
        try:
            if method=='SURFACE':raise CorrespondenceError('Metodo superficie richiesto')
            count,skipped=transfer(working,transform,axis,source_sign,tolerance,selected_only)
        except CorrespondenceError:
            if method=='TOPOLOGY':raise
            chosen='superficie adattata'
            if any(face.hide for face in working.faces):
                raise CorrespondenceError('Mostra tutta la mesh prima del trasferimento di superficie')
            for v,p in zip(working.verts,before):v.co=transform@p
            if selected_only:
                selection_mask=working.verts.layers.float.get('.sculpt_mask') or working.verts.layers.float.new('.sculpt_mask')
                for v in working.verts:
                    if not v.select:v[selection_mask]=1
            path=Path(__file__).with_name('symmetry_surface.py')
            spec=importlib.util.spec_from_file_location('symmetry_surface',path);surface=importlib.util.module_from_spec(spec);spec.loader.exec_module(surface)
            try:frame_positions,info=surface.candidate(working,axis,source_sign,correct_boundary=True,transition_width=transition_width)
            except (surface.SurfaceError,RuntimeError) as ex:raise CorrespondenceError(str(ex)) from ex
            inverse=transform.inverted().to_3x3()
            for v,p,desired in zip(working.verts,before,frame_positions):
                v.co=p+inverse@(desired-transform@p)
            count=info['changed'];skipped=0
        candidate=[v.co.copy() for v in working.verts]
        # All masked/hidden/unselected coordinates are preserved bit for bit, including transforms.
        mask=bm.verts.layers.float.get('.sculpt_mask')
        for i,v in enumerate(vertices):
            if v.hide or (selected_only and not v.select) or (mask and v[mask]>=1):candidate[i]=before[i].copy()
        if not any(a!=b for a,b in zip(before,candidate)):
            return 0,0,dict(method=chosen,strength=1.0)
        bm.faces.index_update()
        old_triangles=[[loop.vert.index for loop in tri] for tri in bm.calc_loop_triangles()]
        old_faces=[tri[0].face.index for tri in bm.calc_loop_triangles()]
        size=max(max(p[k] for p in before)-min(p[k] for p in before) for k in range(3))
        epsilon=max(1e-20,(size*1e-7)**2)
        old_tree=BVHTree.FromPolygons(before,old_triangles,all_triangles=True,epsilon=epsilon)
        old_pairs={tuple(sorted((old_faces[a],old_faces[b]))) for a,b in old_tree.overlap(old_tree) if a<b}
        old_area=[0.]*len(bm.faces)
        for tri,face in zip(old_triangles,old_faces):
            a,b,c=tri;old_area[face]+=(before[b]-before[a]).cross(before[c]-before[a]).length
        accepted=None;strength=1.0
        working.verts.index_update();working.faces.index_update()
        # Keep the full transfer outside collision neighborhoods. Never publish
        # a partial result without rerunning the same whole-mesh validation.
        import heapq
        neighbours=[[] for _ in before];lengths=[]
        for edge in bm.edges:
            a,b=(v.index for v in edge.verts);length=(before[a]-before[b]).length
            neighbours[a].append((b,length));neighbours[b].append((a,length));lengths.append(length)
        collar=max(median(lengths)*6,size*1e-5)
        frozen=set();blend=[1.]*len(before);adapted=0
        for iteration in range(24):
            trial=[a.copy() if a==b or weight==0 else a.lerp(b,weight) for a,b,weight in zip(before,candidate,blend)]
            if not all(all(math.isfinite(x) for x in p) for p in trial):raise CorrespondenceError('Coordinate non finite: nessuna modifica applicata')
            for v,p in zip(working.verts,trial):v.co=p
            working.normal_update()
            loop_triangles=working.calc_loop_triangles()
            triangles=[[loop.vert.index for loop in tri] for tri in loop_triangles];face_ids=[tri[0].face.index for tri in loop_triangles]
            areas=[0.]*len(bm.faces)
            for tri,face in zip(triangles,face_ids):
                a,b,c=tri;areas[face]+=(trial[b]-trial[a]).cross(trial[c]-trial[a]).length
            bad_faces={i for i,(old,new) in enumerate(zip(old_area,areas)) if old>1e-15 and new<old*1e-6}
            tree=BVHTree.FromPolygons(trial,triangles,all_triangles=True,epsilon=epsilon)
            new_pairs={tuple(sorted((face_ids[a],face_ids[b]))) for a,b in tree.overlap(tree) if a<b}
            collisions=new_pairs-old_pairs
            for a,b in collisions:bad_faces.update((a,b))
            print('SHAPE_LOCAL_GUARD',iteration,'collisions',len(collisions),'frozen',len(frozen),flush=True)
            if not bad_faces:
                accepted=trial;adapted=sum(w<1 and a!=b for w,a,b in zip(blend,before,candidate));break
            previous=len(frozen)
            for face in bad_faces:
                frozen.update(v.index for v in bm.faces[face].verts)
            if len(frozen)==previous:break
            distance=[float('inf')]*len(before);queue=[]
            for i in frozen:distance[i]=0.;heapq.heappush(queue,(0.,i))
            while queue:
                d,i=heapq.heappop(queue)
                if d!=distance[i] or d>=collar:continue
                for j,length in neighbours[i]:
                    nd=d+length
                    if nd<distance[j] and nd<collar:
                        distance[j]=nd;heapq.heappush(queue,(nd,j))
            for i,d in enumerate(distance):
                t=min(1.,d/collar)
                blend[i]=t*t*t*(t*(6*t-15)+10)
        if accepted is None:
            raise CorrespondenceError('Il trasferimento creerebbe pieghe o intersezioni: nessuna modifica applicata')
        changed=0
        for v,p,original in zip(vertices,accepted,before):
            if p!=original:v.co=p;changed+=1
        return changed,skipped,dict(method=chosen,strength=strength,adapted_vertices=adapted,protected_collision_vertices=sum(before[i]!=candidate[i] for i in frozen),validation_iterations=iteration+1)
    finally:working.free()


def install(reference_module):
    class MESH_OT_reference_shape(bpy.types.Operator):
        bl_idname='mesh.symmetrize_reference_shape_preview'
        bl_label='Solo forma — maschera protetta'
        bl_options={'REGISTER','UNDO'}
        distance:FloatProperty(name='Distanza iniziale corrispondenze',default=.1,min=.000001,subtype='DISTANCE',description='Usata per inizializzare le coppie; il trasferimento richiede una corrispondenza topologica completa')
        transition_width:FloatProperty(name='Fascia raccordo',default=.15,min=.01,max=1.,subtype='FACTOR',description='Ampiezza del raccordo vicino alla zona protetta; fuori dalla fascia conserva la forma speculare')
        method:EnumProperty(name='Metodo',items=(('AUTO','Automatico','Usa le coppie topologiche quando disponibili; altrimenti adatta la superficie e il bordo mascherato'),('TOPOLOGY','Topologia','Trasferimento per coppie verificate, senza adattamento della superficie'),('SURFACE','Superficie continua','Adatta la forma anche dopo remesh; conserva la topologia di destinazione')),default='AUTO')
        space:EnumProperty(name='Piano',items=(('LOCAL','Assi locali','Usa gli assi locali e l’origine dell’oggetto'),('REFERENCE','Riferimento personalizzato','Usa posizione e rotazione dei piani del gizmo')),default='LOCAL')
        @classmethod
        def poll(cls,c):return c.mode in {'EDIT_MESH','SCULPT'} and c.object and c.object.type=='MESH'
        def invoke(self,c,event):
            s=c.object.sculpt_symref_preview
            self.space='REFERENCE' if s.enabled and (c.mode=='EDIT_MESH' or s.use_for_strokes) else 'LOCAL'
            return c.window_manager.invoke_props_dialog(self)
        def draw(self,c):
            self.layout.prop(self,'method');self.layout.prop(self,'space');self.layout.prop(c.object.sculpt_symref_preview,'edit_direction',text='Lato sorgente')
            if self.method!='SURFACE':self.layout.prop(self,'distance')
            if self.method!='TOPOLOGY':self.layout.prop(self,'transition_width')
            self.layout.label(text='Maschera preservata · geometria controllata',icon='INFO')
            if self.method!='TOPOLOGY':self.layout.label(text='Il bordo mascherato può limitare l’adattamento')
        def execute(self,c):
            ob=c.object;s=ob.sculpt_symref_preview
            sculpt=c.mode=='SCULPT'
            if (not sculpt and len(c.objects_in_mode)!=1) or ob.data.shape_keys or ob.data.users>1 or ob.data.library or any(m.type=='MULTIRES' for m in ob.modifiers) or (sculpt and ob.use_dynamic_topology_sculpting):
                self.report({'ERROR'},'Usa una mesh non condivisa senza shape key o Multires');return {'CANCELLED'}
            frame,valid=(ob.matrix_world,True) if self.space=='LOCAL' else reference_module.reference_matrix(ob,c.evaluated_depsgraph_get())
            if not valid or abs(ob.matrix_world.determinant())<1e-12:
                self.report({'ERROR'},'Riferimento non valido');return {'CANCELLED'}
            if sculpt:
                bpy.ops.object.mode_set(mode='OBJECT')
                bm=bmesh.new();bm.from_mesh(ob.data)
            else:bm=bmesh.from_edit_mesh(ob.data)
            try:
                count,skipped,details=transfer_checked(bm,frame.inverted()@ob.matrix_world,'XYZ'.index(s.edit_direction[-1]),-1 if s.edit_direction.startswith('-') else 1,self.distance,s.edit_selected_only and not sculpt,self.method,self.transition_width)
                if count:
                    bm.normal_update()
                    if sculpt:bm.to_mesh(ob.data);ob.data.update()
                    else:bmesh.update_edit_mesh(ob.data,loop_triangles=True,destructive=False)
            except CorrespondenceError as ex:
                self.report({'ERROR'},str(ex));return {'CANCELLED'}
            finally:
                if sculpt:bm.free();bpy.ops.object.mode_set(mode='SCULPT')
            adapted=details.get('adapted_vertices',0)
            message=f"{count} vertici · {details['method']}"
            if adapted:message+=f" · raccordo locale: {adapted} vertici adattati"
            self.report({'INFO'},message)
            return {'FINISHED'} if count else {'CANCELLED'}
    bpy.utils.register_class(MESH_OT_reference_shape)
    return MESH_OT_reference_shape
