import bmesh
from mathutils import Vector
from mathutils.kdtree import KDTree

def reconstruct(bm, transform, direction, threshold, protect=True, selected=False):
    """Transactional caller-owned BMesh. A masked face and its boundary remain exact."""
    mask=bm.verts.layers.float.get('.sculpt_mask')
    # A small mask value is a soft transfer weight, not permission to freeze an entire polygon.
    # Preserve fully masked faces exactly and send partial masks through the weighted shape
    # transfer path until the constrained topology-transition solver is validated.
    frozen_faces={f for f in bm.faces if f.hide or (selected and not f.select) or (protect and mask and all(v[mask]>=1.0-1e-6 for v in f.verts))}
    partially_masked=bool(protect and mask and any(v[mask]>1e-6 for v in bm.verts))
    frozen_vertices={v for f in frozen_faces for v in f.verts}
    positions={v:v.co.copy() for v in frozen_vertices}
    faces={f:tuple(f.verts) for f in frozen_faces}
    records=[[(v.co.copy(),v[mask] if mask else 0) for v in f.verts] for f in frozen_faces]
    if frozen_faces or partially_masked:
        # If both halves already have identical connectivity, copying positions is the
        # exact geometry-copy result without needlessly deleting and recreating faces.
        import importlib.util
        from pathlib import Path
        spec=importlib.util.spec_from_file_location('verified_topology',Path(__file__).with_name('symmetry_shape_v2.py'))
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        axis='XYZ'.index(direction[-1]);source_sign=-1 if direction.startswith('-') else 1
        saved_mask={v:v[mask] for v in bm.verts} if mask else {}
        if mask:
            for v in frozen_vertices:v[mask]=1
        try:
            changed,_=module.transfer(bm,transform,axis,source_sign,max(threshold,.1),selected)
        except module.CorrespondenceError as ex:
            raise ValueError('The two sides have different topology; mask stitching is not yet supported and no changes were made') from ex
        finally:
            for v,value in saved_mask.items():v[mask]=value
        if any(v.co!=co for v,co in positions.items()):
            raise ValueError('Protected region validation failed; no changes were made')
        return changed, 'shape'
    old_boundary=sum(e.is_boundary for e in bm.edges)
    free=[f for f in bm.faces if f not in frozen_faces]
    if not free:raise ValueError('The entire surface is protected; no changes were made')
    geom=set(free)
    for f in free:geom.update(f.edges);geom.update(f.verts)
    inv=transform.inverted()
    original={v:v.co.copy() for v in bm.verts}
    for v in bm.verts:v.co=transform@v.co
    bmesh.ops.symmetrize(bm,input=list(geom),direction=direction,dist=threshold,use_shapekey=False)
    for v in bm.verts:v.co=inv@v.co
    # Rebuild the original protected patch; native Symmetrize may replace its rim vertices.
    for f in frozen_faces:
        if f.is_valid:bm.faces.remove(f)
    vertices=list(bm.verts);tree=KDTree(len(vertices))
    for i,v in enumerate(vertices):tree.insert(v.co,i)
    tree.balance();created={}
    for record in records:
        loop=[]
        for co,value in record:
            key=tuple(co)
            if key in created:v=created[key]
            else:
                hit=tree.find(co) if vertices else None
                if hit and hit[2]<=1e-6:v=vertices[hit[1]]
                else:v=bm.verts.new(co)
                v.co=co
                if mask:v[mask]=value
                created[key]=v
            loop.append(v)
        try:bm.faces.new(loop)
        except ValueError:raise ValueError('Ambiguous mask stitching; no changes were made')
    for v in list(bm.verts):
        if not v.link_edges:bm.verts.remove(v)
    if any(len(e.link_faces)>2 for e in bm.edges) or sum(e.is_boundary for e in bm.edges)>old_boundary:
        raise ValueError('Mask stitching would leave an open boundary; no changes were made')
    bm.normal_update()
    return len(frozen_faces), 'topology'
