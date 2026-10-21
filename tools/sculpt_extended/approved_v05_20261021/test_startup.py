"""Assert the independently frozen V05 profile and No-REC native UI."""
import bpy,os,sys,json,pathlib,inspect
kit=pathlib.Path(__file__).parent
args=sys.argv[sys.argv.index('--')+1:];mode=args[0];out=pathlib.Path(args[1]);out.mkdir(parents=True,exist_ok=True)
expected=json.loads((kit/'PROFILE.json').read_text())
gold=json.loads((kit/'APPROVED-V05.json').read_text())
assert all(expected[k]==v for k,v in gold.items())
if mode=='override':expected['BLENDER_SCULPT_LAYER_CACHE_MB']='384'
if mode=='optout':
    assert os.getenv('BLENDER_SCULPT_EXTENDED_NATIVE')=='0'
    assert os.getenv('BLENDER_SCULPT_SUBDIVIDE_COMBINED_V05') is None
else:
    for key,value in expected.items():assert os.getenv(key)==value,(key,os.getenv(key),value)
from bl_ui import space_view3d_multires_sidebar as sidebar
from bl_ui import space_view3d_sculpt_extended as extended
assert 'sculpt_layer_recording' not in inspect.getsource(sidebar)
assert 'sculpt_layer_recording' not in inspect.getsource(extended)
names=['VIEW3D_PT_sculpt_extended_multires','VIEW3D_PT_sculpt_multires_layers_sidebar','VIEW3D_PT_multires_polypaint_attributes','VIEW3D_PT_polypaint_blur_v01','VIEW3D_PT_facesets_rc1']
for name in names:assert hasattr(bpy.types,name),name
assert extended.VIEW3D_PT_sculpt_extended_multires.bl_label == 'Draw Transport'
assert extended.VIEW3D_PT_sculpt_extended_multires.bl_parent_id == 'VIEW3D_PT_sculpt_multires_sidebar'
assert extended.VIEW3D_PT_sculpt_extended_multires.bl_category == 'Multires'
assert 'sculpt.multires_base_capture' in inspect.getsource(sidebar.VIEW3D_PT_sculpt_multires_layers_sidebar)
assert hasattr(bpy.types.Brush,'sculpt_draw_transport_v01')
assert hasattr(bpy.types.Scene,'detail_transfer_v01')
from bl_ui import properties_polypaint_bake
assert properties_polypaint_bake.native_available()
for name in ['multires_layer_add','multires_base_capture','multires_detail_transfer_import_v01','multires_polypaint_attribute','polypaint_blur_probe_v01','multires_hr_extract_v05']:
    getattr(bpy.ops.sculpt,name).get_rna_type()
assets=pathlib.Path(bpy.utils.system_resource('DATAFILES'))/'assets/brushes/essentials_brushes-mesh_sculpt.blend'
assert assets.is_file()
result={'status':'PASS','mode':mode,'approved_defaults_verified':len(gold) if mode!='optout' else 0,'No_REC_UI':True,'panels':names,'detail_transfer_copy':True,'native_polypaint_bake':True,'essential_assets':True}
(out/f'startup-{mode}.json').write_text(json.dumps(result,indent=2));print(result)
