"""Inspect saved master-color snapshots in Object Mode without affecting UI Undo history."""
import bpy,json,math,sys
from pathlib import Path
out=Path(sys.argv[sys.argv.index('--')+1]);manifest=json.loads((out/'ui-report.json').read_text())
if manifest.get('status')!='PASS':raise RuntimeError('UI test non completato: '+manifest.get('error','report incompleto'))
report={'status':'RUNNING','snapshots':[],'viewport_size':manifest['viewport_size'],'flags':manifest['flags'],
        'strength_seconds':manifest['strength_seconds'],'paint_seconds':manifest['paint_seconds']}
try:
    for entry in manifest['snapshots']:
        bpy.ops.wm.open_mainfile(filepath=str(out/entry['file']))
        obj=bpy.data.objects['Interactive-Safety-Fixture'];bpy.context.view_layer.objects.active=obj;obj.select_set(True)
        if obj.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        mmd=obj.modifiers['Multires'];mmd.levels=3;bpy.context.view_layer.update()
        ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh()
        try:
            colour=mesh.color_attributes.get('SkinColor')
            if colour is None:raise RuntimeError('SkinColor mancante')
            points=[tuple(v.co) for v in mesh.vertices];colors=[tuple(x.color) for x in colour.data]
            if not points or not colors or not all(math.isfinite(x) for rows in (points,colors) for p in rows for x in p):raise RuntimeError('Dati mancanti o non finiti')
            layers=[(l.name,l.strength,l.visible) for l in mmd.sculpt_layers]
            report['snapshots'].append({'label':entry['label'],'points':points,'colors':colors,'layers':layers})
        finally:ev.to_mesh_clear()
    report['status']='PASS'
finally:(out/'snapshot-report.json').write_text(json.dumps(report),encoding='utf-8')
