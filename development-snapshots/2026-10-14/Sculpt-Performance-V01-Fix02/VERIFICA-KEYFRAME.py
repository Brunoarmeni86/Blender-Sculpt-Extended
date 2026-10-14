"""Native factory-scene integration test. Writes only into the supplied test directory."""
import bpy
import json
import math
from pathlib import Path
import sys

argv = sys.argv[sys.argv.index('--')+1:]
directory = Path(argv[0])
directory.mkdir(parents=True, exist_ok=True)
report = {'status':'RUNNING', 'object_mode':False, 'render_tested':False, 'sculpt_undo_tested':False}

def required(result, label):
    if 'FINISHED' not in result:
        raise RuntimeError(label+': '+str(result))

def sample(obj):
    graph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(graph)
    mmd = next(mod for mod in evaluated.modifiers if mod.type == 'MULTIRES')
    weight = mmd.sculpt_layers['Strength Animation Test'].strength
    mesh = evaluated.to_mesh()
    try:
        points = [tuple(vertex.co) for vertex in mesh.vertices]
        if not points or not all(math.isfinite(x) for p in points for x in p):
            raise RuntimeError('Empty mesh or non-finite evaluated positions')
        return weight, points
    finally:
        evaluated.to_mesh_clear()

def distance(a, b):
    if len(a) != len(b):
        raise RuntimeError('Topology changed during Strength animation')
    return max(abs(x-y) for p,q in zip(a,b) for x,y in zip(p,q))

def near_weight(actual, expected):
    if abs(actual-expected)>1e-5:
        raise RuntimeError(f'Evaluated Strength {actual}, expected {expected}')

try:
    for obj in list(bpy.data.objects):
        if obj.type == 'MESH':
            bpy.data.objects.remove(obj, do_unlink=True)
    required(bpy.ops.mesh.primitive_cube_add(), 'Create cube')
    obj = bpy.context.object
    obj.name = 'Strength-Keyframes-Test'
    mmd = obj.modifiers.new('Multires', 'MULTIRES')
    for _ in range(2):
        required(bpy.ops.object.multires_subdivide(modifier=mmd.name), 'Subdivide')
    mmd.levels = mmd.render_levels = mmd.sculpt_levels = 2
    required(bpy.ops.object.mode_set(mode='SCULPT'), 'Enter Sculpt')
    required(bpy.ops.sculpt.multires_layer_add(), 'Create layer fixture')
    layer = mmd.sculpt_layers[mmd.active_sculpt_layer_index]
    layer.name = 'Strength Animation Test'
    texture = bpy.data.textures.new('Strength Test Gradient', type='BLEND')
    mmd.sculpt_displacement_texture = texture
    mmd.sculpt_displacement_coords = 'LOCAL'
    mmd.sculpt_displacement_strength = .2
    mmd.sculpt_displacement_midlevel = 0.0
    mmd.use_sculpt_displacement = True
    required(bpy.ops.sculpt.multires_texture_displacement_bake(respect_mask=False, disable_live=True), 'Bake nonzero layer fixture')
    required(bpy.ops.object.mode_set(mode='OBJECT'), 'Return to Object')
    report['object_mode'] = True
    scene = bpy.context.scene
    scene.frame_start, scene.frame_end = 1, 21
    for frame, value in ((1,0.0),(11,1.0),(21,0.0)):
        layer.strength = value
        if not layer.keyframe_insert(data_path='strength', frame=frame):
            raise RuntimeError('Strength keyframe insertion failed')
    scene.frame_set(11)
    mmd.sculpt_layer_recording = True
    rec = mmd.sculpt_layer_recording
    if not rec:
        raise RuntimeError('Cannot initialize REC preservation fixture at Strength 1')

    scene.frame_set(1); w0, low = sample(obj); near_weight(w0,0)
    scene.frame_set(11); w1, high = sample(obj); near_weight(w1,1)
    amplitude = distance(low, high)
    if amplitude < 1e-5:
        raise RuntimeError('Strength values changed but evaluated geometry did not change')
    scene.frame_set(6); wm, middle = sample(obj)
    if not 0 < wm < 1:
        raise RuntimeError('Interpolated Strength is outside (0,1)')
    expected = [tuple(x+wm*(y-x) for x,y in zip(a,b)) for a,b in zip(low,high)]
    if distance(middle,expected)>5e-4:
        raise RuntimeError('Interpolated geometry does not match the layer weight')
    scene.frame_set(21); wz, zero = sample(obj); near_weight(wz,0)
    scene.frame_set(1); wb, back = sample(obj); near_weight(wb,0)
    if distance(low,zero)>1e-5 or distance(low,back)>1e-5:
        raise RuntimeError('Repeated frame evaluation changed the zero-weight surface')
    if mmd.sculpt_layer_recording != rec:
        raise RuntimeError('Frame evaluation modified original REC state')
    report.update(keyframes=True, interpolation=True, repeated_frames=True, original_rec_preserved=True,
                  vertices=len(low), max_position_delta=amplitude)

    obj['strength_driver_control'] = .35
    curve = layer.driver_add('strength')
    driver = curve.driver
    driver.type = 'SCRIPTED'
    variable = driver.variables.new()
    variable.name = 'weight'
    variable.type = 'SINGLE_PROP'
    variable.targets[0].id = obj
    variable.targets[0].data_path = '["strength_driver_control"]'
    driver.expression = 'weight'
    scene.frame_set(7)
    wd, driven = sample(obj); near_weight(wd,.35)
    expected = [tuple(x+.35*(y-x) for x,y in zip(a,b)) for a,b in zip(low,high)]
    if distance(driven,expected)>5e-4:
        raise RuntimeError('Driver value changed but geometry did not match')
    if mmd.sculpt_layer_recording != rec:
        raise RuntimeError('Driver evaluation modified original REC state')
    report['driver'] = True

    path = directory/'Strength-Keyframes-Test.blend'
    required(bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False), 'Save test scene')
    required(bpy.ops.wm.open_mainfile(filepath=str(path)), 'Reload test scene')
    obj = bpy.data.objects['Strength-Keyframes-Test']
    if next(mod for mod in obj.modifiers if mod.type=='MULTIRES').sculpt_layer_recording != rec:
        raise RuntimeError('Reload changed original REC state')
    bpy.context.scene.frame_set(7)
    wr, reloaded = sample(obj); near_weight(wr,.35)
    if distance(driven,reloaded)>1e-5:
        raise RuntimeError('Reload changed driven layer geometry')
    report['save_reload'] = True

    # Leave an ordinary keyframed example for interactive inspection.
    mmd = next(mod for mod in obj.modifiers if mod.type=='MULTIRES')
    layer = mmd.sculpt_layers['Strength Animation Test']
    layer.driver_remove('strength')
    bpy.context.scene.frame_set(1)
    near_weight(sample(obj)[0],0)
    required(bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False), 'Save keyframed example')
    report.update(status='PASS', scene=str(path))
    print('STRENGTH_KEYFRAME_NATIVE PASS: geometry, interpolation, frame repeat, driver, REC, save/reload', flush=True)
except Exception as exc:
    report.update(status='FAIL', error=str(exc))
    raise
finally:
    (directory/'strength-keyframes-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
