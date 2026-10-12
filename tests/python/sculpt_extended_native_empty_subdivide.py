"""Small fresh-background fixture for empty-layer subdivision and persistence."""
import argparse
import json
import math
import os
from pathlib import Path
import sys
import time
import bpy


def geometry(ob,mmd):
    mmd.levels=mmd.total_levels;bpy.context.view_layer.update()
    evaluated=ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh=evaluated.to_mesh()
    try:
        values=[list(v.co) for v in mesh.vertices]
        if not all(math.isfinite(x) for v in values for x in v):raise AssertionError('Nonfinite evaluated geometry')
        return values
    finally:evaluated.to_mesh_clear()


def run(output):
    if not bpy.app.background:raise RuntimeError('Use fresh background Blender')
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    bpy.ops.mesh.primitive_cube_add();ob=bpy.context.object
    mmd=ob.modifiers.new('Multires','MULTIRES')
    for _ in range(2):bpy.ops.object.multires_subdivide(modifier=mmd.name)
    mmd.levels=2;mmd.sculpt_levels=2;mmd.render_levels=2
    bpy.ops.object.mode_set(mode='SCULPT');bpy.context.view_layer.update()
    for _ in range(3):
        if bpy.ops.sculpt.multires_layer_add()!={'FINISHED'}:raise AssertionError('Layer add failed')
    bpy.ops.object.mode_set(mode='OBJECT')
    start=time.perf_counter()
    if bpy.ops.object.multires_subdivide(modifier=mmd.name)!={'FINISHED'}:raise AssertionError('Subdivide operator failed')
    elapsed=time.perf_counter()-start
    if mmd.total_levels!=3 or len(mmd.sculpt_layers)!=3:raise AssertionError('Subdivision did not preserve stack/level')
    before=geometry(ob,mmd)
    scene_path=output.with_suffix('.blend');bpy.ops.wm.save_as_mainfile(filepath=str(scene_path))
    bpy.ops.wm.open_mainfile(filepath=str(scene_path));ob=bpy.context.object;mmd=ob.modifiers.get('Multires')
    after=geometry(ob,mmd)
    if before!=after:
        if len(before)!=len(after) or max(abs(a-b) for v,w in zip(before,after) for a,b in zip(v,w))>1e-6:
            raise AssertionError('Save/reload changed geometry')
    output.write_text(json.dumps(dict(blender=bpy.app.version_string,
        empty_subdivide=os.environ.get('BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE','0'),
        levels=mmd.total_levels,layers=len(mmd.sculpt_layers),seconds=elapsed,
        positions=after,scope='Empty layers only; checks level, stack, finite shape and persistence. No populated-layer or Undo test.'),indent=2))
    print('EMPTY_LAYER_SUBDIVIDE_NATIVE_COMPLETE: '+str(output))

def compare(folder):
    a=json.loads((folder/'native-empty-subdivide-0.json').read_text())
    b=json.loads((folder/'native-empty-subdivide-1.json').read_text())
    if a['layers']!=b['layers'] or a['levels']!=b['levels'] or len(a['positions'])!=len(b['positions']):
        raise AssertionError('Different geometry/stack layout')
    difference=max(abs(x-y) for v,w in zip(a['positions'],b['positions']) for x,y in zip(v,w))
    result=dict(vertices=len(a['positions']),layers=b['layers'],levels=b['levels'],
                max_coordinate_difference=difference,seconds_previous=a['seconds'],seconds_sparse=b['seconds'],
                scope='Small empty-layer fixture and persistence only; timings are descriptive. Does not validate Commit, populated layers, Undo or peak RSS.')
    (folder/'native-empty-subdivide-comparison.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
    if difference>1e-6:raise AssertionError('Sparse subdivision changed empty-layer geometry')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--compare',type=Path)
    args=p.parse_args(sys.argv[sys.argv.index('--')+1:])
    if args.compare:compare(args.compare)
    elif args.output:run(args.output)
    else:p.error('--output or --compare required')
