"""Temporary read-only fingerprint buttons. No scene/Undo changes or saved properties."""
import bpy
from bpy.props import EnumProperty

_context = {'case': None}

class SCULPT_OT_smooth_layer_verify(bpy.types.Operator):
    bl_idname = 'sculpt.smooth_layer_verify'
    bl_label = 'Verifica Smooth Layer'
    bl_description = 'Legge impronte di base, layer memorizzati e superficie, senza Commit o Undo'
    bl_options = set()
    stage: EnumProperty(items=[
        ('EMPTY', 'Prima: layer vuoto', ''),
        ('FILLED', 'Prima: layer con deformazione', ''),
        ('AFTER', 'Dopo Smooth', ''),
        ('UNDO', 'Dopo Undo', ''),
        ('REDO', 'Dopo Redo', ''),
        ('COMMIT', 'Dopo Commit', ''),
        ('SWITCH', 'Dopo cambio layer e ritorno', ''),
        ('REOPEN', 'Dopo riapertura', ''),
    ])

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj is not None and obj.type == 'MESH' and any(m.type == 'MULTIRES' for m in obj.modifiers)

    def execute(self, context):
        case = _context['case']
        if self.stage in {'EMPTY', 'FILLED'}:
            case = self.stage
            stage = 'BEFORE'
        else:
            if _context['case'] is None:
                self.report({'ERROR'}, 'Premi prima uno dei due pulsanti Prima')
                return {'CANCELLED'}
            stage = self.stage
        label = 'SMOOTH_%s_%s' % (case, stage)
        result = bpy.ops.sculpt.multires_base_fingerprint(
            label=label, include_layer_payloads=True, include_surface=True)
        if result != {'FINISHED'}:
            self.report({'ERROR'}, 'Impronta non completata; termina pennellate o filtri attivi')
            return {'CANCELLED'}
        _context['case'] = case
        print('[SMOOTH_LAYER_VERIFY_V02] label=%s object=%s' % (label, context.active_object.name), flush=True)
        self.report({'INFO'}, 'Impronta registrata: ' + label)
        return {'FINISHED'}

class VIEW3D_PT_smooth_layer_verify(bpy.types.Panel):
    bl_label = 'Verifica Smooth (temporanea)'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'

    def draw(self, context):
        col = self.layout.column(align=True)
        col.label(text='Legge dati; non esegue Undo o Commit')
        col.operator(SCULPT_OT_smooth_layer_verify.bl_idname, text='Prima: layer vuoto').stage = 'EMPTY'
        col.operator(SCULPT_OT_smooth_layer_verify.bl_idname, text='Prima: layer con deformazione').stage = 'FILLED'
        col.separator()
        for stage, text in [('AFTER', 'Dopo Smooth'), ('UNDO', 'Dopo Undo'), ('REDO', 'Dopo Redo'), ('COMMIT', 'Dopo Commit'), ('SWITCH', 'Dopo cambio layer e ritorno'), ('REOPEN', 'Dopo riapertura')]:
            col.operator(SCULPT_OT_smooth_layer_verify.bl_idname, text=text).stage = stage

for cls in (SCULPT_OT_smooth_layer_verify, VIEW3D_PT_smooth_layer_verify):
    old = getattr(bpy.types, cls.__name__, None)
    if old is not None:
        bpy.utils.unregister_class(old)
    bpy.utils.register_class(cls)
print('[SMOOTH_LAYER_VERIFY_V02] pronto: N > Multires > Verifica Smooth; impronte complete solo su richiesta', flush=True)
