"""Native A/B result producer; executed in separate processes with identical fixtures."""
import bpy
import ctypes
import json
import math
import os
from pathlib import Path
import sys
import time

out=Path(sys.argv[sys.argv.index('--')+1]);out.mkdir(parents=True,exist_ok=True)
report={'status':'RUNNING','cases':[],'compact':os.getenv('BLENDER_SCULPT_SUBDIVIDE_COMPACT'),
        'precision':os.getenv('BLENDER_SCULPT_SUBDIVIDE_PRESERVE_RETAINED')}

def required(result,label):
    if 'FINISHED' not in result:raise RuntimeError(label+': '+str(result))

def sampled(obj):
    bpy.context.view_layer.update()
    ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh()
    try:
        pts=[tuple(v.co) for v in mesh.vertices]
        if not pts or not all(math.isfinite(x) for p in pts for x in p):raise RuntimeError('Invalid geometry')
        colour=mesh.color_attributes.get('SkinColor')
        if colour is None or not len(colour.data):raise RuntimeError('Color Attribute missing')
        colours=[tuple(x.color) for x in colour.data]
        if not all(math.isfinite(x) for p in colours for x in p):raise RuntimeError('Invalid color')
        return {'points':pts,'colors':colours}
    finally:ev.to_mesh_clear()

def peak_bytes():
    if os.name!='nt':return None
    class PMC(ctypes.Structure):
        _fields_=[('cb',ctypes.c_uint32),('PageFaultCount',ctypes.c_uint32)]+[(k,ctypes.c_size_t) for k in ('PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage')]
    get=ctypes.windll.kernel32.GetCurrentProcess;get.restype=ctypes.c_void_p
    query=ctypes.windll.psapi.GetProcessMemoryInfo;query.argtypes=[ctypes.c_void_p,ctypes.POINTER(PMC),ctypes.c_uint32];query.restype=ctypes.c_int
    data=PMC();data.cb=ctypes.sizeof(data)
    return int(data.PeakWorkingSetSize) if query(get(),ctypes.byref(data),data.cb) else None

def fixture(kind):
    if bpy.context.object and bpy.context.object.mode!='OBJECT':required(bpy.ops.object.mode_set(mode='OBJECT'),'Exit mode')
    for obj in list(bpy.data.objects):bpy.data.objects.remove(obj,do_unlink=True)
    required(bpy.ops.mesh.primitive_cube_add(),'Cube');obj=bpy.context.object;obj.name='PerformanceFixture';mesh=obj.data
    colours=mesh.color_attributes.new(name='SkinColor',type='FLOAT_COLOR',domain='CORNER')
    mesh.color_attributes.active_color_index=list(mesh.color_attributes).index(colours)
    for i,x in enumerate(colours.data):x.color=(.15+.01*(i%3),.25,.35,1.)
    face_sets=mesh.attributes.new('.sculpt_face_set','INT','FACE')
    for i,x in enumerate(face_sets.data):x.value=1+i%3
    mask=mesh.attributes.new('.sculpt_mask','FLOAT','POINT')
    for i,x in enumerate(mask.data):x.value=float(i%3)*.2
    mmd=obj.modifiers.new('Multires','MULTIRES')
    for _ in range(2):required(bpy.ops.object.multires_subdivide(modifier=mmd.name),'Initial subdivision')
    mmd.levels=mmd.sculpt_levels=mmd.render_levels=2
    required(bpy.ops.object.mode_set(mode='SCULPT'),'Enter Sculpt')
    for i in range(4):
        required(bpy.ops.sculpt.multires_layer_add(),'Add layer');layer=mmd.sculpt_layers[mmd.active_sculpt_layer_index];layer.name='Layer-'+str(i)
        if kind=='many_active' or (kind!='all_empty' and i==0):
            tex=bpy.data.textures.new('Gradient-'+str(i),type='BLEND')
            mmd.sculpt_displacement_texture=tex;mmd.sculpt_displacement_coords='LOCAL';mmd.sculpt_displacement_strength=.12+.02*i
            mmd.sculpt_displacement_midlevel=0.;mmd.use_sculpt_displacement=True
            required(bpy.ops.sculpt.multires_texture_displacement_bake(respect_mask=False,disable_live=True),'Bake layer fixture')
    required(bpy.ops.object.mode_set(mode='OBJECT'),'Return Object')
    for i,layer in enumerate(mmd.sculpt_layers):
        layer.strength=(.65,-.25,0.,1.3)[i]
        if kind=='hidden_active' and i==0:layer.visible=False
    if kind=='allocated_zeros':
        # Produce allocated zero grids with the old path, in both A/B processes.
        saved={k:os.environ.get(k) for k in ('BLENDER_SCULPT_SUBDIVIDE_COMPACT','BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE')}
        try:
            os.environ['BLENDER_SCULPT_SUBDIVIDE_COMPACT']='0';os.environ['BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE']='0'
            required(bpy.ops.object.multires_subdivide(modifier=mmd.name),'Allocate legacy zero layer fixture')
        finally:
            for k,v in saved.items():
                if v is None:os.environ.pop(k,None)
                else:os.environ[k]=v
    return obj,mmd

try:
    for kind in ('all_empty','one_active','allocated_zeros','many_active','hidden_active'):
        for mode in ('CATMULL_CLARK','SIMPLE','LINEAR'):
            obj,mmd=fixture(kind);old_level=mmd.total_levels;mmd.levels=old_level
            before=sampled(obj)
            mask_before=[x.value for x in obj.data.attributes['.sculpt_mask'].data]
            faces_before=[x.value for x in obj.data.attributes['.sculpt_face_set'].data]
            metadata=[(l.name,l.strength,l.visible) for l in mmd.sculpt_layers]
            begun=time.perf_counter();required(bpy.ops.object.multires_subdivide(modifier=mmd.name,mode=mode),'Measured subdivision')
            elapsed=time.perf_counter()-begun
            if mmd.total_levels!=old_level+1:raise RuntimeError('Subdivision did not advance')
            if [(l.name,l.strength,l.visible) for l in mmd.sculpt_layers]!=metadata:raise RuntimeError('Layer metadata changed')
            mmd.levels=old_level;retained=sampled(obj);mmd.levels=old_level+1;high=sampled(obj)
            if [x.value for x in obj.data.attributes['.sculpt_mask'].data]!=mask_before or [x.value for x in obj.data.attributes['.sculpt_face_set'].data]!=faces_before:
                raise RuntimeError('Mask/Face Sets changed during subdivision')
            original_mask=[x.value for x in obj.data.attributes['.sculpt_mask'].data]
            original_faces=[x.value for x in obj.data.attributes['.sculpt_face_set'].data]
            for layer in mmd.sculpt_layers:
                layer.visible=False;layer.strength=1.
            base_only=sampled(obj)
            variants=[]
            for index in range(4):
                for i,l in enumerate(mmd.sculpt_layers):l.visible=(i==index)
                isolated=sampled(obj)
                if kind=='many_active' or (kind!='all_empty' and index==0):
                    delta=max(abs(x-y) for p,q in zip(base_only['points'],isolated['points']) for x,y in zip(p,q))
                    if delta<1e-5:raise RuntimeError('Fixture layer has no displacement')
                variants.append(isolated)
            mask_values=[x.value for x in obj.data.attributes['.sculpt_mask'].data]
            face_values=[x.value for x in obj.data.attributes['.sculpt_face_set'].data]
            path=out/(kind+'-'+mode+'.blend')
            required(bpy.ops.wm.save_as_mainfile(filepath=str(path),check_existing=False),'Save staged layers')
            required(bpy.ops.wm.open_mainfile(filepath=str(path)),'Reload staged layers')
            obj=bpy.data.objects['PerformanceFixture']
            reloaded=sampled(obj)
            loaded_modifier=obj.modifiers['Multires']
            if [(l.name,l.strength,l.visible) for l in loaded_modifier.sculpt_layers]!=[(name,1.,i==3) for i,(name,_,_) in enumerate(metadata)]:
                raise RuntimeError('Layer metadata not preserved on reload')
            if [x.value for x in obj.data.attributes['.sculpt_mask'].data]!=original_mask or [x.value for x in obj.data.attributes['.sculpt_face_set'].data]!=original_faces:
                raise RuntimeError('Mask/Face Sets changed on reload')
            row={'kind':kind,'mode':mode,'old_level':old_level,'seconds':elapsed,'peak_working_set_bytes':peak_bytes(),
                 'before':before,'retained':retained,'high':high,'variants':variants,'reloaded':reloaded,
                 'metadata':metadata,'mask':mask_values,'face_sets':face_values}
            report['cases'].append(row)
            print(f'SUBDIVIDE_AB_CASE compact={report["compact"]} kind={kind} mode={mode} seconds={elapsed:.6f}',flush=True)
    report.update(status='PASS',process_peak_working_set_bytes=peak_bytes())
except Exception as error:
    report.update(status='FAIL',error=str(error));raise
finally:
    (out/'subdivide-ab.json').write_text(json.dumps(report),encoding='utf-8')
