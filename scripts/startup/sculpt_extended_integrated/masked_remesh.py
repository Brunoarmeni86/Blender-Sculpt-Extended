"""Local surface remeshing with immutable masked faces; independent attribute option."""
import bpy,bmesh,math
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from bpy.props import BoolProperty, FloatProperty

def _core(bm,voxel,iterations=24,coarsening=3.0,falloff=1.0):
    bm.verts.index_update();bm.faces.index_update();bm.normal_update()
    original_boundary_edges=sum(e.is_boundary for e in bm.edges)
    original_overfull_edges=sum(len(e.link_faces)>2 for e in bm.edges)
    old_tree=BVHTree.FromBMesh(bm);old_pairs={tuple(sorted((a+1,b+1))) for a,b in old_tree.overlap(old_tree) if a<b}
    mask=bm.verts.layers.float.get('.sculpt_mask')
    if not mask:raise ValueError('No mask')
    # A partly masked face is a transition face, not fully frozen.
    # Only a face with every vertex fully masked may be frozen bit-for-bit.
    full_mask=1.0-1e-6
    protected=[all(v[mask]>=full_mask for v in f.verts) for f in bm.faces]
    protect=bm.faces.layers.int.new('.local_protected');face_id=bm.faces.layers.int.new('.local_face_id');vertex_id=bm.verts.layers.int.new('.local_vertex_id')
    for i,f in enumerate(bm.faces):f[protect]=protected[i];f[face_id]=i+1
    frozen={v for f in bm.faces if f[protect] for v in f.verts}|{v for v in bm.verts if v.is_boundary or v[mask]>=full_mask}
    for i,v in enumerate(bm.verts):v[vertex_id]=i+1 if v in frozen else 0
    saved={v[vertex_id]:tuple(v.co) for v in bm.verts if v[vertex_id]}
    saved_faces={f[face_id]:tuple(v[vertex_id] for v in f.verts) for f in bm.faces if f[protect]}
    source_triangles=[tri for tri in bm.calc_loop_triangles() if not tri[0].face[protect]]
    positions=[v.co.copy() for v in bm.verts];surface=BVHTree.FromPolygons(positions,[[loop.vert.index for loop in tri] for tri in source_triangles],all_triangles=True)
    free=[f for f in bm.faces if not f[protect]];bmesh.ops.triangulate(bm,faces=free,quad_method='BEAUTY',ngon_method='BEAUTY')
    # Smoothly relax the edge-density target toward the masked area.
    # Topology operations remain discrete; fully protected topology is immutable.
    target=voxel*math.sqrt(2);rows=[]
    coarsening=max(1.0,min(6.0,float(coarsening)))
    falloff=max(0.25,min(4.0,float(falloff)))
    def edge_target(edge):
        m=max(0.0,min(1.0,(edge.verts[0][mask]+edge.verts[1][mask])*.5))
        t=m*m*(3.0-2.0*m)
        return target*(1.0+(coarsening-1.0)*t**falloff)
    for iteration in range(iterations):
        protect=bm.faces.layers.int['.local_protected'];vertex_id=bm.verts.layers.int['.local_vertex_id']
        free=[f for f in bm.faces if not f[protect]];edges={e for f in free for e in f.edges}
        long=[e for e in edges if e.is_manifold and e.calc_length()>edge_target(e)*4/3 and all(not f[protect] for f in e.link_faces)]
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
        candidates=sorted((e for e in edges if e.is_manifold and e.calc_length()<edge_target(e)*.75 and all(not v[vertex_id] for v in e.verts) and all(len(f.verts)==3 and not f[protect] for f in e.link_faces)),key=lambda e:e.calc_length())
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
                local_target=target*(1.0+(coarsening-1.0)*max(0.0,min(1.0,v[mask]))**falloff)
                if delta.length>local_target*.25:delta*=local_target*.25/delta.length
                influence=(1.0-max(0.0,min(1.0,v[mask])))**falloff
                point,normal,face,distance=surface.find_nearest(v.co+delta*.35*influence)
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
    final_boundary_edges=sum(e.is_boundary for e in bm.edges)
    final_overfull_edges=sum(len(e.link_faces)>2 for e in bm.edges)
    if final_boundary_edges>original_boundary_edges or final_overfull_edges>original_overfull_edges:
        raise RuntimeError('Remeshing would introduce open boundaries or non-manifold edges; no changes were applied')
    info=dict(protected_vertices=len(saved),protected_faces=len(saved_faces),final_vertices=len(bm.verts),final_faces=len(bm.faces),iterations=rows,new_intersections=len(pairs-old_pairs),nonmanifold_edges=sum(not e.is_manifold for e in bm.edges),boundary_edges=final_boundary_edges,overfull_edges=final_overfull_edges,inconsistent_edges=sum(len(e.link_loops)==2 and e.link_loops[0].vert==e.link_loops[1].vert for e in bm.edges),protected_coordinates_exact=True,protected_topology_exact=True)
    # Report geometric diagnostics without rejecting the user's voxel size.
    return bm,info


def local_remesh(mesh,voxel,coarsening=3.0,falloff=1.0):
    if any(a.name in {'.local_protected','.local_face_id','.local_vertex_id'} for a in mesh.attributes):
        raise ValueError('Temporary attributes already exist; no changes were applied')
    bm=bmesh.new()
    try:
        bm.from_mesh(mesh)
        return _core(bm,voxel,coarsening=coarsening,falloff=falloff)
    except Exception:
        bm.free();raise

class OBJECT_OT_voxel_remesh_protected_preview(bpy.types.Operator):
    bl_idname='object.voxel_remesh_protected_preview'
    bl_label='Local Mask-Protected Remesh'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,c):return c.object and c.object.type=='MESH' and c.mode in {'SCULPT','OBJECT'}
    def execute(self,c):
        ob=c.object;source=ob.data
        if source.users!=1 or source.library or source.shape_keys or any(m.type=='MULTIRES' for m in ob.modifiers) or (ob.mode=='SCULPT' and ob.use_dynamic_topology_sculpting):
            self.report({'ERROR'},'Use a single-user mesh without shape keys, Multires or Dyntopo');return {'CANCELLED'}
        if source.remesh_voxel_size<=0:
            self.report({'ERROR'},'Invalid voxel size');return {'CANCELLED'}
        mask=source.attributes.get('.sculpt_mask')
        if mask is None:
            self.report({'INFO'},'No mask to protect');return {'CANCELLED'}
        if all(all(mask.data[i].value>=1.0-1e-6 for i in face.vertices) for face in source.polygons):
            self.report({'INFO'},'The mask protects the entire surface; no changes were made');return {'CANCELLED'}
        sculpt=ob.mode=='SCULPT';bm=None;stage=None;backup=None;committed=False
        try:
            if sculpt:bpy.ops.object.mode_set(mode='OBJECT')
            bm,info=local_remesh(source,source.remesh_voxel_size,
                                 source.sculpt_remesh_mask_coarsening,
                                 source.sculpt_remesh_mask_falloff)
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
            self.report({'INFO'},f"Local remesh: {info['protected_vertices']} protected vertices, editable region updated")
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
        self.layout.prop(c.object.data,'sculpt_remesh_protect_mask_preview',text='Protect Masked Region')
        if c.object.data.sculpt_remesh_protect_mask_preview:
            col=self.layout.column(align=True)
            col.prop(c.object.data,'sculpt_remesh_mask_coarsening',text='Transition Density Scale')
            col.prop(c.object.data,'sculpt_remesh_mask_falloff',text='Mask Influence Curve')

def install():
    bpy.types.Mesh.sculpt_remesh_mask_coarsening=FloatProperty(name='Transition Density Scale',description='Relative edge size in the editable masked transition; 1 means uniform density',min=1.0,max=6.0,default=3.0)
    bpy.types.Mesh.sculpt_remesh_mask_falloff=FloatProperty(name='Mask Influence Curve',description='Controls the transition between full remeshing and the protected region',min=.25,max=4.0,default=1.0)
    bpy.types.Mesh.sculpt_remesh_protect_mask_preview=BoolProperty(name='Protect Masked Region',get=lambda mesh: bool(mesh.get('sculpt_remesh_protect_mask_preview',False)),set=lambda mesh,value: mesh.__setitem__('sculpt_remesh_protect_mask_preview',bool(value)),description='Locally remesh only the unmasked region while preserving coordinates and faces in fully or partially masked areas. Does not fuse separate volumes. The mask is retained independently of other attributes')
    bpy.utils.register_class(OBJECT_OT_voxel_remesh_protected_preview)
    from bl_ui import space_view3d_toolbar
    space_view3d_toolbar.VIEW3D_PT_sculpt_voxel_remesh.append(draw_protection)

def uninstall():
    from bl_ui import space_view3d_toolbar
    space_view3d_toolbar.VIEW3D_PT_sculpt_voxel_remesh.remove(draw_protection)
    bpy.utils.unregister_class(OBJECT_OT_voxel_remesh_protected_preview)
    del bpy.types.Mesh.sculpt_remesh_protect_mask_preview
    del bpy.types.Mesh.sculpt_remesh_mask_coarsening
    del bpy.types.Mesh.sculpt_remesh_mask_falloff
