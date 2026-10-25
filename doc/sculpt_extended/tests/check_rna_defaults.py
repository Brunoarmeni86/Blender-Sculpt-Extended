import bpy,json,os
from pathlib import Path
operators={
 'multires_polypaint_attribute':['name','new_name'],
 'multires_detail_transfer_import_v01':['preview_object'],
 'multires_polypaint_uv_bake_v01':['attribute','uv_map'],
}
result={}
config=bpy.context.window_manager.keyconfigs.new('RNA default probe')
keymap=config.keymaps.new(name='RNA default probe')
for name,props in operators.items():
 op=getattr(bpy.ops.sculpt,name)
 rna=op.get_rna_type()
 instance=keymap.keymap_items.new('sculpt.'+name,'A','PRESS').properties
 for prop in props:
  p=rna.properties[prop]
  assert p.default=='' and getattr(instance,prop)==''
  setattr(instance,prop,'RNA_probe')
  assert getattr(instance,prop)=='RNA_probe'
  setattr(instance,prop,'')
  assert getattr(instance,prop)==''
  result[name+'.'+prop]={'default':p.default,'maxlen':p.length_max,'type':p.type,'explicit_and_empty_values':'PASS'}
Path(os.environ['RNA_TEST_RESULT']).write_text(json.dumps(result,indent=2))
print('RNA_DEFAULTS_PASS',len(result))

