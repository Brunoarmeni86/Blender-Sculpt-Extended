import bpy
import json
from pathlib import Path
import sys

folder = Path(sys.argv[sys.argv.index('--') + 1])
bpy.ops.wm.open_mainfile(filepath=str(folder/'Conform-Native.blend'))
ob = bpy.data.objects['Conform-Undo-Cache-Fixture']
m = ob.modifiers['Multires']
old = m.levels
points = {}
for level in range(m.total_levels+1):
    m.levels = level
    bpy.context.view_layer.update()
    evaluated = ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        points[str(level)] = [list(v.co) for v in mesh.vertices]
    finally:
        evaluated.to_mesh_clear()
m.levels = old
report = {'status':'PASS','points':points,'sculpt':m.sculpt_levels,'total':m.total_levels,
          'layers':[{'name':l.name,'strength':l.strength,'visible':l.visible} for l in m.sculpt_layers]}
(folder/'reload-test.json').write_text(json.dumps(report), encoding='utf-8')
print('UNDO_BAKE_RELOAD READ PASS', flush=True)
