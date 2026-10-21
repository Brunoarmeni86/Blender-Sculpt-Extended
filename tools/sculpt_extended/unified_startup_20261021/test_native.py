import bpy,os,sys,json,pathlib
args=sys.argv[sys.argv.index('--')+1:];mode=args[0];out=pathlib.Path(args[1]);out.mkdir(parents=True,exist_ok=True)
expected=json.loads(pathlib.Path(__file__).with_name('native-defaults.json').read_text())
if mode=='override':expected.update(BLENDER_SCULPT_LAYER_CACHE_MB='256',BLENDER_SCULPT_WEIGHT_MULTI_CACHE='0')
if mode=='optout':
    assert os.environ['BLENDER_SCULPT_EXTENDED_NATIVE']=='0'
    assert os.getenv('BLENDER_SCULPT_LAYER_BAKE_BASE') is None
else:
    for key,value in expected.items():assert os.getenv(key)==value,(key,os.getenv(key),value)
names=('VIEW3D_PT_sculpt_multires_sidebar','VIEW3D_PT_sculpt_multires_layers_sidebar','MULTIRES_OT_polypaint_manage','VIEW3D_PT_multires_polypaint_attributes','VIEW3D_PT_sculpt_extended_multires','SCULPT_OT_layer_diagnostics')
for name in names:assert hasattr(bpy.types,name),name
report={'mode':mode,'status':'PASS','native_defaults_checked':len(expected) if mode!='optout' else 0,'registered_types':list(names),'startup_external_script_required':False}
if mode=='normal':
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    bpy.ops.mesh.primitive_cube_add()
    obj=bpy.context.object;m=obj.modifiers.new('Native smoke','MULTIRES')
    for _ in range(2):assert bpy.ops.object.multires_subdivide(modifier=m.name,mode='CATMULL_CLARK')=={'FINISHED'}
    bpy.ops.object.mode_set(mode='SCULPT')
    assert bpy.ops.sculpt.multires_layer_add()=={'FINISHED'}
    assert len(m.sculpt_layers)==1
    m.sculpt_layers[0].name='Native startup layer';m.sculpt_layers[0].strength=.65
    assert bpy.ops.sculpt.multires_layer_duplicate()=={'FINISHED'}
    assert len(m.sculpt_layers)==2
    m.sculpt_layers[1].visible=False
    bpy.ops.object.mode_set(mode='OBJECT')
    path=out/'native-smoke.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    bpy.ops.wm.open_mainfile(filepath=str(path))
    m=bpy.context.object.modifiers['Native smoke']
    assert m.total_levels==2 and len(m.sculpt_layers)==2 and not m.sculpt_layers[1].visible
    report['geometry_smoke']='PASS: two subdivisions, layer add/duplicate, hidden layer, save/reopen'
(out/f'test-{mode}.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
