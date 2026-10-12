"""Two native bakes, no render: audit Vector Tangent against Vector Object.

The analytical candidate is computed in Python, not implemented in the baker.
Cycles source reference: kernel/svm/displace.h at c044b9c1.
"""
import argparse
import hashlib
import itertools
import json
import math
import os
import pathlib
import sys

import bpy
from mathutils import Matrix, Vector


def stats(rows):
    meaningful=[r for r in rows if r['object_length']>1e-6]
    if not meaningful:raise AssertionError('No nonzero displacement samples')
    def rms(key):return math.sqrt(sum(r[key]**2 for r in meaningful)/len(meaningful))
    return dict(samples=len(rows),nonzero_samples=len(meaningful),
                current_position_error_rms=rms('position_error'),
                current_relative_error_rms=rms('relative_error'),
                current_angle_rms_degrees=rms('angle_degrees'),
                current_angle_max_degrees=max(r['angle_degrees'] for r in meaningful),
                current_length_error_percent_rms=100*rms('length_relative_error'),
                analytical_candidate_position_error_rms=rms('candidate_position_error'))


def main(output):
    if not bpy.app.background:raise RuntimeError('Run in fresh background Blender')
    if os.environ.get('BLENDER_MULTIRES_BAKE_SS')!='4':raise RuntimeError('This audit requires SS=4')
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=1,radius=1,calc_uvs=False)
    ob=bpy.context.object;mesh=ob.data
    for v in mesh.vertices:
        v.co.x*=1.6;v.co.y*=.8;v.co.z*=1.2
    mesh.update()
    for p in mesh.polygons:p.use_smooth=True
    uv=mesh.uv_layers.new(name='VectorFrameProbe');uv.active=True;uv.active_render=True
    flat=[0.0]*(2*len(mesh.loops));triangles=[]
    for i,p in enumerate(mesh.polygons):
        loops=list(p.loop_indices)
        if len(loops)!=3:raise AssertionError('Original faces must be triangles')
        x,y=i%5,i//5
        coords=[((x+.12)/5,(y+.12)/4),((x+.88)/5,(y+.12)/4),((x+.12)/5,(y+.88)/4)]
        if i%2:coords[1],coords[2]=coords[2],coords[1]
        for loop,st in zip(loops,coords):flat[2*loop:2*loop+2]=st
        triangles.append((loops,coords))
    if hasattr(uv,'uv'):uv.uv.foreach_set('vector',flat)
    else:uv.data.foreach_set('uv',flat)
    mesh.calc_tangents(uvmap=uv.name)
    frames=[]
    for loops,coords in triangles:
        frames.append(dict(uv=coords,normals=[list(mesh.vertices[mesh.loops[i].vertex_index].normal) for i in loops],
                           tangents=[list(mesh.loops[i].tangent) for i in loops],
                           signs=[mesh.loops[i].bitangent_sign for i in loops]))
    mmd=ob.modifiers.new('Multires','MULTIRES')
    for _ in range(3):bpy.ops.object.multires_subdivide(modifier=mmd.name)
    mmd.levels=0;mmd.render_levels=3
    material=bpy.data.materials.new('VectorFrameProbe');material.use_nodes=True
    mesh.materials.append(material);node=material.node_tree.nodes.new('ShaderNodeTexImage')
    material.node_tree.nodes.active=node
    settings=bpy.context.scene.render.bake
    settings.use_multires=True;settings.use_clear=True;settings.margin=0
    settings.type='VECTOR_DISPLACEMENT';settings.use_lores_mesh=True
    pixels={}
    for space in ('OBJECT','TANGENT'):
        image=bpy.data.images.new('Vector-'+space,width=256,height=256,alpha=True,float_buffer=True)
        image.colorspace_settings.name='Non-Color';image.alpha_mode='STRAIGHT'
        node.image=image;settings.displacement_space=space
        if bpy.ops.object.bake_image('EXEC_DEFAULT')!={'FINISHED'}:raise AssertionError('Bake failed: '+space)
        data=list(image.pixels[:])
        if not all(math.isfinite(v) for v in data):raise AssertionError('Nonfinite bake: '+space)
        pixels[space]=data
    rows=[]
    # Unequal weights avoid CCG quadrant diagonals and full footprints stay inside.
    probes=list(itertools.permutations((.7,.2,.1)))
    for face,frame in enumerate(frames):
        coords=frame['uv']
        for requested in probes:
            st=[sum(requested[i]*coords[i][j] for i in range(3)) for j in range(2)]
            x,y=[int(v*256) for v in st];center=((x+.5)/256,(y+.5)/256)
            a,b,c=coords
            det=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
            w0=((b[1]-c[1])*(center[0]-c[0])+(c[0]-b[0])*(center[1]-c[1]))/det
            w1=((c[1]-a[1])*(center[0]-c[0])+(a[0]-c[0])*(center[1]-c[1]))/det
            weights=(w0,w1,1-w0-w1)
            def interp(name):return Vector([sum(weights[i]*frame[name][i][j] for i in range(3)) for j in range(3)])
            # Cycles: normalized object N, raw interpolated T, normalized cross(N,T).
            normal=interp('normals').normalized();tangent=interp('tangents')
            sign=sum(weights[i]*frame['signs'][i] for i in range(3))
            bitangent=normal.cross(tangent).normalized()*sign
            from_cycles=Matrix((tangent,normal,bitangent)).transposed()
            try:to_cycles=from_cycles.inverted()
            except ValueError:raise AssertionError('Singular probe frame')
            values={space:pixels[space][4*(y*256+x):4*(y*256+x)+4] for space in pixels}
            if any(abs(v[3]-1)>1e-6 for v in values.values()):raise AssertionError('Missing interior coverage')
            expected=Vector(values['OBJECT'][:3]);stored=Vector(values['TANGENT'][:3])
            decoded=from_cycles@stored
            # Hypothesis only: invert the exact Cycles frame, preserving vector magnitude.
            candidate=to_cycles@expected;candidate_decoded=from_cycles@candidate
            length=expected.length;err=(decoded-expected).length
            angle=math.degrees(decoded.angle(expected)) if min(decoded.length,length)>1e-9 else 0.0
            rows.append(dict(face=face,mirrored=bool(face%2),pixel=[x,y],object=list(expected),
                             tangent=list(stored),cycles_decoded=list(decoded),object_length=length,
                             position_error=err,relative_error=err/length if length>1e-9 else 0.0,
                             angle_degrees=angle,length_relative_error=(decoded.length-length)/length if length>1e-9 else 0.0,
                             candidate_tangent=list(candidate),candidate_position_error=(candidate_decoded-expected).length))
    report=dict(blender=bpy.app.version_string,ss='4',active_uv=mesh.uv_layers.active.name,
                frame_sha256=hashlib.sha256(json.dumps(frames,sort_keys=True).encode()).hexdigest(),
                summary=stats(rows),regular=stats([r for r in rows if not r['mirrored']]),
                mirrored=stats([r for r in rows if r['mirrored']]),samples=rows,
                scope='Same native texels: Vector Object control vs Vector Tangent decoded by the Cycles equation on original smooth triangles with identity object transform. No renderer, geometry reconstruction, filtering or seam test. The Python candidate is an analytical inverse, not a compiled bake implementation or independent quality oracle.')
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(report,indent=2))
    print(json.dumps({k:report[k] for k in ('summary','regular','mirrored')},indent=2))
    print('VECTOR_FRAME_AUDIT_COMPLETE: '+str(output))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=pathlib.Path,required=True)
    args=parser.parse_args(sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else [])
    main(args.output)
