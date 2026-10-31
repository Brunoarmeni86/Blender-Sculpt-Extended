"""Native Cycles diagnostic for existing Multires bake CPU patches.
No production changes. Uses the already-tested ellipsoid/UV probe scene.
"""
import argparse
import array
import hashlib
import importlib.util
import json
import math
import os
import pathlib
import sys


def render_pixels(scene, folder, name):
    import bpy
    path = folder / (name + '.exr')
    scene.render.filepath = str(path)
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '32'
    if bpy.ops.render.render(write_still=True) != {'FINISHED'}:
        raise AssertionError('Cycles render failed: ' + name)
    image = bpy.data.images.load(str(path), check_existing=False)
    image.colorspace_settings.name = 'Non-Color'
    pixels = array.array('f', image.pixels[:])
    if len(pixels) != 256*256*4 or not all(math.isfinite(x) for x in pixels):
        raise AssertionError('Invalid rendered image: ' + name)
    with (folder / (name + '.f32')).open('wb') as f:
        pixels.tofile(f)
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_depth = '8'
    image.save_render(str(folder / (name + '.png')), scene=scene)
    bpy.data.images.remove(image)
    return dict(name=name, float_sha256=hashlib.sha256(pixels.tobytes()).hexdigest())


def encode_normal(nodes, links, normal_socket, output):
    emission = nodes.new('ShaderNodeEmission')
    mul = nodes.new('ShaderNodeVectorMath'); mul.operation = 'SCALE'
    mul.inputs['Scale'].default_value = .5
    add = nodes.new('ShaderNodeVectorMath'); add.operation = 'ADD'
    add.inputs[1].default_value = (.5,.5,.5)
    links.new(normal_socket, mul.inputs[0]); links.new(mul.outputs[0],add.inputs[0])
    links.new(add.outputs[0],emission.inputs['Color'])
    links.new(emission.outputs[0],output.inputs['Surface'])


def texture_seam_metrics(mesh, normal_image, images, uv_name):
    """Compare shared-edge samples from separate islands after padding.

    Values use bilinear filtering at exact edge UVs; partial coverage and padding
    can extrapolate slightly. These jumps are diagnostic, not zero assertions.
    """
    mesh.calc_tangents(uvmap=uv_name)
    uv=mesh.uv_layers.get(uv_name)
    coords=[tuple(x.vector) for x in uv.uv] if hasattr(uv,'uv') else [tuple(x.uv) for x in uv.data]
    buffers={name:list(image.pixels[:]) for name,image in dict(normal=normal_image,**images).items()}
    def filtered(name,st):
        data=buffers[name];x=st[0]*256-.5;y=st[1]*256-.5
        ix=math.floor(x);iy=math.floor(y);fx=x-ix;fy=y-iy
        return [sum(data[4*(min(255,max(0,iy+dy))*256+min(255,max(0,ix+dx)))+c]
                    *(fx if dx else 1-fx)*(fy if dy else 1-fy)
                    for dy in (0,1) for dx in (0,1)) for c in range(4)]
    edges={}
    for poly in mesh.polygons:
        loops=list(poly.loop_indices)
        for a,b in zip(loops,loops[1:]+loops[:1]):
            key=tuple(sorted((mesh.loops[a].vertex_index,mesh.loops[b].vertex_index)))
            pair=(a,b) if mesh.loops[a].vertex_index==key[0] else (b,a)
            edges.setdefault(key,[]).append(pair)
    def unit(v):
        l=math.sqrt(sum(x*x for x in v))
        if l<1e-12:raise AssertionError('Degenerate seam normal')
        return [x/l for x in v]
    jumps=[];object_jumps=[];scalar_jumps=[]
    for edge,pairs in edges.items():
        if len(pairs)!=2:continue
        for amount in (.25,.5,.75):
            decoded=[];vectors=[];scalars=[]
            for a,b in pairs:
                st=[(1-amount)*coords[a][j]+amount*coords[b][j] for j in range(2)]
                rgb=filtered('normal',st)
                if rgb[3]<.99:raise AssertionError('Padding missed seam sample')
                n=[(1-amount)*mesh.vertices[edge[0]].normal[j]+amount*mesh.vertices[edge[1]].normal[j] for j in range(3)]
                t=[(1-amount)*mesh.loops[a].tangent[j]+amount*mesh.loops[b].tangent[j] for j in range(3)]
                sign=-1 if (1-amount)*mesh.loops[a].bitangent_sign+amount*mesh.loops[b].bitangent_sign<0 else 1
                bit=[sign*(n[1]*t[2]-n[2]*t[1]),sign*(n[2]*t[0]-n[0]*t[2]),sign*(n[0]*t[1]-n[1]*t[0])]
                v=[2*x-1 for x in rgb[:3]]
                decoded.append(unit([t[j]*v[0]+bit[j]*v[1]+n[j]*v[2] for j in range(3)]))
                vectors.append(filtered('vector-object',st)[:3]);scalars.append(filtered('scalar',st)[0])
            jumps.append(math.degrees(math.acos(max(-1,min(1,sum(a*b for a,b in zip(*decoded)))))))
            object_jumps.append(math.sqrt(sum((a-b)**2 for a,b in zip(*vectors))))
            scalar_jumps.append(abs(scalars[0]-scalars[1]))
    def stats(values):return dict(samples=len(values),rms=math.sqrt(sum(x*x for x in values)/len(values)),maximum=max(values))
    return dict(normal_angle_degrees=stats(jumps),vector_object_units=stats(object_jumps),scalar_normalized=stats(scalar_jumps),
                scope='Bilinear shared-edge jumps after padding; no zero-error assertion and no scalar physical scale assumption.')


def run(folder):
    import bpy
    from mathutils import Vector
    folder.mkdir(parents=True,exist_ok=True)
    probe_path = pathlib.Path(__file__).with_name('sculpt_extended_multires_bake_native_normal_tbn.py')
    spec=importlib.util.spec_from_file_location('tbn_probe',probe_path)
    probe=importlib.util.module_from_spec(spec);spec.loader.exec_module(probe)
    probe.bake(folder/'native-normal-probe.json')
    ob=bpy.context.object;mesh=ob.data;mmd=ob.modifiers.get('Multires')
    material=mesh.materials[0]
    active_node=material.node_tree.nodes.active
    normal_image=active_node.image
    # Repeat the same normal bake with gutter padding for render texture filtering.
    settings=bpy.context.scene.render.bake
    settings.margin=8
    bpy.ops.object.bake_image('EXEC_DEFAULT')
    images={}
    for name,space in (('vector-object','OBJECT'),('vector-tangent','TANGENT'),('scalar','OBJECT')):
        image=bpy.data.images.new(name,width=256,height=256,alpha=True,float_buffer=True)
        image.colorspace_settings.name='Non-Color';image.alpha_mode='STRAIGHT'
        active_node.image=image;settings.type='DISPLACEMENT' if name=='scalar' else 'VECTOR_DISPLACEMENT'
        settings.displacement_space=space;settings.use_lores_mesh=True
        if bpy.ops.object.bake_image('EXEC_DEFAULT') != {'FINISHED'}:
            raise AssertionError('Bake failed: '+name)
        pixels=array.array('f',image.pixels[:])
        if not all(math.isfinite(v) for v in pixels):raise AssertionError('Non-finite bake: '+name)
        images[name]=image
        image.file_format='OPEN_EXR';image.filepath_raw=str(folder/(name+'.exr'));image.save()
    normal_image.file_format='OPEN_EXR';normal_image.filepath_raw=str(folder/'normal.exr');normal_image.save()
    seams=texture_seam_metrics(mesh,normal_image,images,mesh.uv_layers.active.name)
    # High geometry is the actual evaluated Multires target, not a guessed surface.
    mmd.levels=3;bpy.context.view_layer.update()
    high_mesh=bpy.data.meshes.new_from_object(ob.evaluated_get(bpy.context.evaluated_depsgraph_get()))
    high=bpy.data.objects.new('HighReference',high_mesh);bpy.context.collection.objects.link(high)
    high.matrix_world=ob.matrix_world.copy()
    mmd.levels=0
    high_material=bpy.data.materials.new('HighNormalReference');high_material.use_nodes=True
    high_mesh.materials.clear();high_mesh.materials.append(high_material)
    nodes=high_material.node_tree.nodes;nodes.clear();links=high_material.node_tree.links
    output=nodes.new('ShaderNodeOutputMaterial');geom=nodes.new('ShaderNodeNewGeometry')
    encode_normal(nodes,links,geom.outputs['Normal'],output)
    scene=bpy.context.scene;scene.render.engine='CYCLES';scene.cycles.device='CPU'
    scene.cycles.samples=16;scene.cycles.use_denoising=False;scene.cycles.seed=0
    scene.cycles.use_adaptive_sampling=False
    scene.render.resolution_x=256;scene.render.resolution_y=256;scene.render.resolution_percentage=100
    scene.render.film_transparent=True
    scene.world.color=(0,0,0)
    camera_data=bpy.data.cameras.new('ProbeCamera');camera=bpy.data.objects.new('ProbeCamera',camera_data)
    bpy.context.collection.objects.link(camera);camera.location=(3,-4,3)
    camera.rotation_euler=(-Vector(camera.location)).to_track_quat('-Z','Y').to_euler()
    camera_data.type='ORTHO';camera_data.ortho_scale=4.5;scene.camera=camera
    results=[]
    ob.hide_render=True;high.hide_render=False
    results.append(render_pixels(scene,folder,'high-reference'))
    high.hide_render=True;ob.hide_render=False;mmd.show_render=False;mmd.show_viewport=False
    nodes=material.node_tree.nodes;nodes.clear();links=material.node_tree.links
    material.displacement_method='DISPLACEMENT'
    output=nodes.new('ShaderNodeOutputMaterial');tex=nodes.new('ShaderNodeTexImage');tex.image=normal_image
    tex.interpolation='Linear';tex.extension='EXTEND'
    normal=nodes.new('ShaderNodeNormalMap');normal.space='TANGENT';normal.uv_map=mesh.uv_layers.active.name
    # Fork RNA names this option "base"; inspect enum values rather than guessing.
    prop=normal.bl_rna.properties.get('base')
    if prop is None:raise AssertionError('Normal Map Original Base setting unavailable')
    original=[i.identifier for i in prop.enum_items if 'ORIGINAL' in i.identifier.upper()]
    if len(original)!=1:raise AssertionError('Cannot identify Original Base enum')
    normal.base=original[0]
    links.new(tex.outputs['Color'],normal.inputs['Color'])
    encode_normal(nodes,links,normal.outputs['Normal'],output)
    results.append(render_pixels(scene,folder,'normal-only'))
    simple=ob.modifiers.new('ReconstructionSampling','SUBSURF');simple.subdivision_type='SIMPLE'
    simple.levels=4;simple.render_levels=4
    vector=nodes.new('ShaderNodeVectorDisplacement');vector.inputs['Midlevel'].default_value=0
    vector.inputs['Scale'].default_value=1
    vector_tex=nodes.new('ShaderNodeTexImage');vector_tex.interpolation='Linear';vector_tex.extension='EXTEND'
    links.new(vector_tex.outputs['Color'],vector.inputs['Vector'])
    links.new(vector.outputs['Displacement'],output.inputs['Displacement'])
    for name,space in (('paired-object','OBJECT'),('paired-tangent','TANGENT')):
        vector_tex.image=images['vector-object' if space=='OBJECT' else 'vector-tangent'];vector.space=space
        results.append(render_pixels(scene,folder,name))
    bpy.ops.wm.save_as_mainfile(filepath=str(folder/'reconstruction.blend'))
    report=dict(blender=bpy.app.version_string,cycles_tbn=os.environ.get('BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN','1'),
                renders=results,seams=seams,normal_base=normal.base,scalar_note='Scalar map exported only: normalization scale not exported by native bake API; not guessed.',
                scope='High Multires geometry vs normal-only and vector-displaced low mesh. Diagnostic, no assumed error threshold. Simple subdivision tangent frames and filtering can introduce additional error.')
    (folder/'render-report.json').write_text(json.dumps(report,indent=2))


def load_pixels(path):
    a=array.array('f')
    with path.open('rb') as f:a.fromfile(f,256*256*4)
    if len(a)!=256*256*4 or not all(math.isfinite(x) for x in a):
        raise AssertionError('Invalid float image: '+str(path))
    return a


def metrics(a,b,mask):
    angles=[];rgb_sq=0
    for i in mask:
        av=[2*a[4*i+j]-1 for j in range(3)];bv=[2*b[4*i+j]-1 for j in range(3)]
        al=math.sqrt(sum(v*v for v in av));bl=math.sqrt(sum(v*v for v in bv))
        if min(al,bl)<1e-8:continue
        cosine=sum(x*y for x,y in zip(av,bv))/(al*bl)
        angles.append(math.degrees(math.acos(max(-1,min(1,cosine)))))
        rgb_sq+=sum((a[4*i+j]-b[4*i+j])**2 for j in range(3))
    if not angles:raise AssertionError('Empty render comparison')
    return dict(pixels=len(angles),angle_rms_degrees=math.sqrt(sum(x*x for x in angles)/len(angles)),
                angle_max_degrees=max(angles),rgb_rms=math.sqrt(rgb_sq/(3*len(angles))))


def saved_reference_fixture(folder):
    """Audit geometry and camera in already-rendered scenes, without rendering."""
    try:
        import bpy
    except ImportError:
        return None
    path=folder/'reconstruction.blend'
    if not path.exists():return None
    with bpy.data.libraries.load(str(path),link=False) as (source,target):
        for name in ('HighReference','ProbeCamera'):
            if name not in source.objects:raise AssertionError('Missing reference object: '+name)
        target.objects=['HighReference','ProbeCamera']
    high,camera=target.objects
    try:
        mesh=high.data
        payload=dict(vertices=[list(v.co) for v in mesh.vertices],
                     faces=[list(p.vertices) for p in mesh.polygons],
                     smooth=[p.use_smooth for p in mesh.polygons],
                     normals=[list(v.normal) for v in mesh.vertices],
                     high_matrix=[list(row) for row in high.matrix_world],
                     camera_matrix=[list(row) for row in camera.matrix_world],
                     camera_type=camera.data.type,ortho_scale=camera.data.ortho_scale,
                     lens=camera.data.lens)
        return hashlib.sha256(json.dumps(payload,sort_keys=True).encode()).hexdigest()
    finally:
        for ob in (high,camera):
            data=ob.data;bpy.data.objects.remove(ob,do_unlink=True)
            if data.users==0:
                if isinstance(data,bpy.types.Mesh):bpy.data.meshes.remove(data)
                else:bpy.data.cameras.remove(data)


def compare(folder):
    old=folder/'previous';new=folder/'corrected'
    reference=load_pixels(new/'high-reference.f32')
    reference_old=load_pixels(old/'high-reference.f32')
    mode_reports={mode:json.loads((folder/mode/'render-report.json').read_text()) for mode in ('previous','corrected')}
    if [mode_reports[m]['cycles_tbn'] for m in ('previous','corrected')] != ['0','1']:
        raise AssertionError('Unexpected TBN modes')
    fixtures={mode:saved_reference_fixture(folder/mode) for mode in ('previous','corrected')}
    known=all(fixtures.values());matching=(len(set(fixtures.values()))==1) if known else None
    reference_mask=[i for i in range(256*256) if min(reference[4*i+3],reference_old[4*i+3])>.999]
    differences=[abs(a-b) for a,b in zip(reference,reference_old)]
    ref_metrics=metrics(reference_old,reference,reference_mask)
    ref_metrics.update(max_abs_rgba=max(differences),rgba_rms=math.sqrt(sum(x*x for x in differences)/len(differences)),
                       channels_over_1e_6=sum(x>1e-6 for x in differences),
                       alpha_max_abs=max(abs(reference[4*i+3]-reference_old[4*i+3]) for i in range(256*256)),
                       fixture_hashes=fixtures,geometry_and_camera_match=matching)
    report={'reference_drift':ref_metrics}
    if matching is False:
        report['validation_status']='needs_scene_mismatch_review'
    elif max(differences)>1e-6:
        report['validation_status']='needs_reference_drift_review'
    elif matching is None:
        report['validation_status']='reference_pixels_match_fixture_not_audited'
    else:
        report['validation_status']='reference_control_consistent'
    for name in ('normal-only','paired-object','paired-tangent'):
        a=load_pixels(old/(name+'.f32'));b=load_pixels(new/(name+'.f32'))
        # A common mask includes BOTH references and both candidate images.
        mask=[i for i in range(256*256) if min(reference[4*i+3],reference_old[4*i+3],a[4*i+3],b[4*i+3])>.999]
        report[name]=dict(previous_vs_high=metrics(a,reference_old,mask),corrected_vs_high=metrics(b,reference,mask),
                          previous_vs_corrected=metrics(a,b,mask),
                          previous_vs_corrected_high_control=metrics(a,reference,mask),
                          corrected_vs_previous_high_control=metrics(b,reference_old,mask))
    report['seams']={mode:mode_reports[mode]['seams'] for mode in ('previous','corrected')}
    report['scope']='Diagnostic only. Each mode uses its own high reference; cross-control metrics and reference drift are reported separately. Reference drift must be reviewed before concluding an A/B improvement. Screen-space geometry correspondence and scalar scale remain unvalidated.'
    (folder/'cycles-reconstruction-comparison.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=pathlib.Path);parser.add_argument('--compare',type=pathlib.Path)
    args=parser.parse_args(sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else [])
    if args.compare:compare(args.compare)
    elif args.output:run(args.output)
    else:parser.error('--output or --compare required')
