"""Local surface remeshing with immutable masked faces; independent attribute option."""
import bpy,bmesh,math
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from bpy.props import BoolProperty

def _core(bm,voxel,iterations=24):
    bm.verts.index_update();bm.faces.index_update();bm.normal_update()
    old_tree=BVHTree.FromBMesh(bm);old_pairs={tuple(sorted((a+1,b+1))) for a,b in old_tree.overlap(old_tree) if a<b}
    mask=bm.verts.layers.float.get('.sculpt_mask')
    if not mask:raise ValueError('No mask')
    protected=[any(v[mask]>1e-6 for v in f.verts) for f in bm.faces]
    protect=bm.faces.layers.int.new('.local_protected');face_id=bm.faces.layers.int.new('.local_face_id');vertex_id=bm.verts.layers.int.new('.local_vertex_id')
    for i,f in enumerate(bm.faces):f[protect]=protected[i];f[face_id]=i+1
    frozen={v for f in bm.faces if f[protect] for v in f.verts}|{v for v in bm.verts if v.is_boundary}
    for i,v in enumerate(bm.verts):v[vertex_id]=i+1 if v in frozen else 0
    saved={v[vertex_id]:tuple(v.co) for v in bm.verts if v[vertex_id]}
    saved_faces={f[face_id]:tuple(v[vertex_id] for v in f.verts) for f in bm.faces if f[protect]}
    source_triangles=[tri for tri in bm.calc_loop_triangles() if not tri[0].face[protect]]
    positions=[v.co.copy() for v in bm.verts];surface=BVHTree.FromPolygons(positions,[[loop.vert.index for loop in tri] for tri in source_triangles],all_triangles=True)
    free=[f for f in bm.faces if not f[protect]];bmesh.ops.triangulate(bm,faces=free,quad_method='BEAUTY',ngon_method='BEAUTY')
    target=voxel*math.sqrt(2);rows=[]
    for iteration in range(iterations):
        protect=bm.faces.layers.int['.local_protected'];vertex_id=bm.verts.layers.int['.local_vertex_id']
        free=[f for f in bm.faces if not f[protect]];edges={e for f in free for e in f.edges}
        long=[e for e in edges if e.is_manifold and e.calc_length()>target*4/3 and all(not f[protect] for f in e.link_faces)]
        if long:
            split=bmesh.ops.subdivide_edges(bm,edges=long,cuts=1,use_grid_fill=True)
            vertex_id=bm.verts.layers.int['.local_vertex_id']
            for key in ('geom_inner','geom_split'):
                for v in split.get(key,[]):
                    if isinstance(v,bmesh.types.BMVert):v[vertex_id]=0
            protect=bm.faces.layers.int['.local_protected'];free=[f for f in bm.faces if not f[protect]]
            bmesh.ops.triangulate(bm,faces=free,quad_method='BEAUTY',ngon_method='BEAUTY')
        protect=bm.faces.layers.int['.local_protected'];vertex_id=bm.verts.layers.int['.local_vertex_id']
        free=[f for f in bm.faces if not f[protect]];edges={e for f in free for e in f.edges}
        candidates=sorted((e for e in edges if e.is_manifold and e.calc_length()<target*.75 and all(not v[vertex_id] for v in e.verts) and all(len(f.verts)==3 for f in e.link_faces)),key=lambda e:e.calc_length())
        selected=[];avoid=set()
        for e in candidates:
            a,b=e.verts
            if a in avoid or b in avoid:continue
            na={edge.other_vert(a) for edge in a.link_edges};nb={edge.other_vert(b) for edge in b.link_edges}
            if len(na&nb)!=2:continue
            selected.append(e);avoid.update(na|nb|{a,b})
        if selected:bmesh.ops.collapse(bm,edges=selected,uvs=False)
        vertex_id=bm.verts.layers.int['.local_vertex_id'];protect=bm.faces.layers.int['.local_protected']
        if iteration%3==2:
            free=[f for f in bm.faces if not f[protect]];edges={e for f in free for e in f.edges if e.is_manifold and all(not neighbour[protect] for neighbour in e.link_faces)}
            if edges:bmesh.ops.beautify_fill(bm,faces=free,edges=list(edges),use_restrict_tag=False,method='AREA')
            bm.normal_update();vertex_id=bm.verts.layers.int['.local_vertex_id'];updates=[]
            for v in bm.verts:
                if v[vertex_id]:continue
                neighbours=[edge.other_vert(v) for edge in v.link_edges]
                if not neighbours:continue
                delta=sum((n.co for n in neighbours),Vector())/len(neighbours)-v.co;delta-=v.normal*delta.dot(v.normal)
                if delta.length>target*.25:delta*=target*.25/delta.length
                point,normal,face,distance=surface.find_nearest(v.co+delta*.35)
                if point is not None:updates.append((v,point))
            for v,point in updates:v.co=point
        rows.append(dict(iteration=iteration,split=len(long),collapsed=len(selected),verts=len(bm.verts)))
        if not selected and not long:break
    bm.normal_update();bm.verts.index_update();bm.faces.index_update()
    vertex_id=bm.verts.layers.int['.local_vertex_id'];face_id=bm.faces.layers.int['.local_face_id'];protect=bm.faces.layers.int['.local_protected']
    assert {v[vertex_id]:tuple(v.co) for v in bm.verts if v[vertex_id]}==saved,'Frozen vertices changed'
    assert {f[face_id]:tuple(v[vertex_id] for v in f.verts) for f in bm.faces if f[protect]}==saved_faces,'Protected faces changed'
    final_tree=BVHTree.FromBMesh(bm);labels=[f[face_id] if f[face_id] else -f.index-1 for f in bm.faces]
    pairs={tuple(sorted((labels[a],labels[b]))) for a,b in final_tree.overlap(final_tree) if a<b}
    info=dict(protected_vertices=len(saved),protected_faces=len(saved_faces),final_vertices=len(bm.verts),final_faces=len(bm.faces),iterations=rows,new_intersections=len(pairs-old_pairs),nonmanifold_edges=sum(not e.is_manifold for e in bm.edges),inconsistent_edges=sum(len(e.link_loops)==2 and e.link_loops[0].vert==e.link_loops[1].vert for e in bm.edges),protected_coordinates_exact=True,protected_topology_exact=True)
    # Report geometric diagnostics without rejecting the user's voxel size.
    return bm,info


def local_remesh(mesh,voxel):
    if any(a.name in {'.local_protected','.local_face_id','.local_vertex_id'} for a in mesh.attributes):
        raise ValueError('Attributi temporanei già presenti; nessuna modifica applicata')
    bm=bmesh.new()
    try:
        bm.from_mesh(mesh)
        return _core(bm,voxel)
    except Exception:
        bm.free();raise

class OBJECT_OT_voxel_remesh_protected_preview(bpy.types.Operator):
    bl_idname='object.voxel_remesh_protected_preview'
    bl_label='Remesh locale protetto dalla maschera'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,c):return c.object and c.object.type=='MESH' and c.mode in {'SCULPT','OBJECT'}
    def execute(self,c):
        ob=c.object;source=ob.data
        if source.users!=1 or source.library or source.shape_keys or any(m.type=='MULTIRES' for m in ob.modifiers) or (ob.mode=='SCULPT' and ob.use_dynamic_topology_sculpting):
            self.report({'ERROR'},'Usa una mesh non condivisa senza shape key, Multires o Dyntopo');return {'CANCELLED'}
        if source.remesh_voxel_size<=0:
            self.report({'ERROR'},'Dimensione voxel non valida');return {'CANCELLED'}
        mask=source.attributes.get('.sculpt_mask')
        if mask is None:
            self.report({'INFO'},'Nessuna maschera da proteggere');return {'CANCELLED'}
        if all(any(mask.data[i].value>1e-6 for i in face.vertices) for face in source.polygons):
            self.report({'INFO'},'La maschera protegge tutta la superficie: nessuna modifica');return {'CANCELLED'}
        sculpt=ob.mode=='SCULPT';bm=None;stage=None;backup=None;committed=False
        try:
            if sculpt:bpy.ops.object.mode_set(mode='OBJECT')
            bm,info=local_remesh(source,source.remesh_voxel_size)
            stage=source.copy();stage.name='__protected_remesh_result'
            bm.to_mesh(stage);bm.free();bm=None
            for name in ('.local_protected','.local_face_id','.local_vertex_id'):
                attribute=stage.attributes.get(name)
                if attribute:stage.attributes.remove(attribute)
            if not source.use_remesh_preserve_attributes:
                # Mask protection retains its own mask; the other attributes follow the checkbox.
                for attribute in list(stage.attributes):
                    if not attribute.is_required and attribute.name!='.sculpt_mask':stage.attributes.remove(attribute)
            stage.update();backup=source.copy()
            publication=bmesh.new()
            try:publication.from_mesh(stage);publication.to_mesh(source)
            finally:publication.free()
            committed=True;source.update();ob.update_tag(refresh={'DATA'})
            if sculpt:bpy.ops.object.mode_set(mode='SCULPT')
            self.report({'INFO'},f"Remesh locale: {info['protected_vertices']} vertici protetti, zona libera aggiornata")
            return {'FINISHED'}
        except (ValueError,RuntimeError,AssertionError) as ex:
            if committed and backup:
                if ob.mode=='SCULPT':bpy.ops.object.mode_set(mode='OBJECT')
                rollback=bmesh.new()
                try:rollback.from_mesh(backup);rollback.to_mesh(source);source.update()
                finally:rollback.free()
                committed=False
            self.report({'ERROR'},str(ex));return {'CANCELLED'}
        finally:
            if bm:bm.free()
            if stage and stage.users==0:bpy.data.meshes.remove(stage)
            if backup and backup.users==0:bpy.data.meshes.remove(backup)
            if sculpt and ob.mode!='SCULPT':bpy.ops.object.mode_set(mode='SCULPT')

def draw_protection(self,c):
    if c.object and c.object.type=='MESH':
        self.layout.separator()
        self.layout.prop(c.object.data,'sculpt_remesh_protect_mask_preview',text='Proteggi zona mascherata')

def install():
    bpy.types.Mesh.sculpt_remesh_protect_mask_preview=BoolProperty(name='Proteggi zona mascherata',get=lambda mesh: bool(mesh.get('sculpt_remesh_protect_mask_preview',False)),set=lambda mesh,value: mesh.__setitem__('sculpt_remesh_protect_mask_preview',bool(value)),description='Remesh locale solo della zona senza maschera: conserva coordinate e facce della zona mascherata, anche parziale. Non fonde volumi separati. La maschera viene mantenuta indipendentemente dagli altri attributi')
    bpy.utils.register_class(OBJECT_OT_voxel_remesh_protected_preview)
    from bl_ui import space_view3d_toolbar
    space_view3d_toolbar.VIEW3D_PT_sculpt_voxel_remesh.append(draw_protection)

def uninstall():
    from bl_ui import space_view3d_toolbar
    space_view3d_toolbar.VIEW3D_PT_sculpt_voxel_remesh.remove(draw_protection)
    bpy.utils.unregister_class(OBJECT_OT_voxel_remesh_protected_preview)
    del bpy.types.Mesh.sculpt_remesh_protect_mask_preview
