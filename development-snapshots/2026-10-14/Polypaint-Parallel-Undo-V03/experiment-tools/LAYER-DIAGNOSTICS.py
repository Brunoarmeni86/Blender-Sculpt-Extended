"""Read original object/modifier state; no Undo step or evaluated high-res mesh."""
import bpy
import json
import os
from pathlib import Path
import time

class SCULPT_OT_layer_diagnostics(bpy.types.Operator):
    bl_idname = 'sculpt.layer_diagnostics'
    bl_label = 'Sculpt Layer Diagnostics'
    bl_description = 'Export modifier stack and layer metadata without changing the scene'
    bl_options = {'REGISTER'}

    def execute(self, context):
        ob = context.object
        if ob is None or ob.type != 'MESH':
            self.report({'ERROR'}, 'Seleziona la mesh usata nella prova')
            return {'CANCELLED'}
        modifiers = []
        for m in ob.modifiers:
            item = dict(name=m.name, type=m.type, show_viewport=m.show_viewport,
                        show_render=m.show_render, show_in_editmode=m.show_in_editmode)
            for prop in ('object', 'target', 'mirror_object'):
                target = getattr(m, prop, None)
                if isinstance(target, bpy.types.ID):
                    item[prop] = target.name
            if m.type == 'MULTIRES':
                item.update(sculpt_level=m.sculpt_levels, total_level=m.total_levels,
                            active_layer=getattr(m, 'active_sculpt_layer_index', None),
                            recording=getattr(m, 'sculpt_layer_recording', None),
                            layers=[dict(name=l.name, strength=l.strength, visible=l.visible)
                                    for l in getattr(m, 'sculpt_layers', ())])
            modifiers.append(item)
        flags = ('BLENDER_SCULPT_COMMIT_KEEP_CCG', 'BLENDER_SCULPT_SKIP_EMPTY_BM_FLUSH',
                 'BLENDER_SCULPT_UNDO_BOUNDARY_METADATA', 'BLENDER_SCULPT_LAYER_STACK_CHECK_V3',
                 'BLENDER_SCULPT_LAYER_READONLY_WEIGHT', 'BLENDER_SCULPT_LAYER_UNDO_CONTROLS')
        result = dict(blender=bpy.app.version_string, object=ob.name, mode=ob.mode,
                      original_vertices=len(ob.data.vertices), original_polygons=len(ob.data.polygons),
                      mesh_users=ob.data.users, modifiers=modifiers,
                      flags={key: os.environ.get(key) for key in flags},
                      scope='Original object and RNA metadata only; no high-res export, scene mutation or Undo push')
        path = Path(__file__).with_name(f'layer-diagnostics-{time.time_ns()}.json')
        path.write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(f'[LAYER_DIAGNOSTICS] saved={path.name}', flush=True)
        self.report({'INFO'}, f'Diagnostica salvata: {path.name}')
        return {'FINISHED'}

if not hasattr(bpy.types, 'SCULPT_OT_layer_diagnostics'):
    bpy.utils.register_class(SCULPT_OT_layer_diagnostics)
print('F3: Sculpt Layer Diagnostics. Esporta la stack senza modificare la scena o lo storico Undo.', flush=True)
