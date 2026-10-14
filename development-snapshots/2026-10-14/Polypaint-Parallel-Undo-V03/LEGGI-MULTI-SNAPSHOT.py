"""Evaluate every color attribute in each saved scene; do not alter tested UI history."""
import bpy,json,math,sys
from pathlib import Path
out=Path(sys.argv[sys.argv.index('--')+1]);manifest=json.loads((out/'ui-report.json').read_text())
if manifest.get('status')!='PASS':raise RuntimeError(manifest.get('error','UI incomplete'))
report={'status':'RUNNING','snapshots':[]}
try:
    for entry in manifest['snapshots']:
        bpy.ops.wm.open_mainfile(filepath=str(out/entry['file']))
        obj=bpy.data.objects['Interactive-Safety-Fixture'];bpy.context.view_layer.objects.active=obj;obj.select_set(True)
        if obj.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        mmd=obj.modifiers['Multires'];mmd.levels=3;bpy.context.view_layer.update()
        levels={a.name:a.level for a in mmd.polypaint_attributes}
        if mmd.polypaint_attribute:levels[mmd.polypaint_attribute]=mmd.polypaint_level
        if levels!=entry['levels']:raise RuntimeError('Master levels changed on reload: '+entry['label'])
        active=obj.data.color_attributes.active_color
        if (active.name if active else '')!=entry['active']:raise RuntimeError('Active attribute changed on reload')
        ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh()
        try:
            colors={a.name:[tuple(x.color) for x in a.data] for a in mesh.color_attributes}
            points=[tuple(v.co) for v in mesh.vertices]
            if not colors or not points:raise RuntimeError('Empty evaluation')
            for name,rows in colors.items():
                if not rows or not all(math.isfinite(v) for row in rows for v in row):raise RuntimeError('Non-finite color '+name)
            if not all(math.isfinite(v) for row in points for v in row):raise RuntimeError('Non-finite geometry')
            report['snapshots'].append({'label':entry['label'],'colors':colors,'points':points,
                'levels':levels,'active':entry['active'],
                'layers':[(l.name,l.strength,l.visible) for l in mmd.sculpt_layers]})
        finally:ev.to_mesh_clear()
    report['status']='PASS'
finally:(out/'snapshot-report.json').write_text(json.dumps(report),encoding='utf-8')
