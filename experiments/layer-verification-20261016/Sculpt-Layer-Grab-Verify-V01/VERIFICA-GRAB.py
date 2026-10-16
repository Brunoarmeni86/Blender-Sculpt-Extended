"""Temporary read-only fingerprint buttons. No scene/Undo changes or saved properties."""
import bpy
from bpy.props import EnumProperty

_context = {'case': None, 'run': 0}

class SCULPT_OT_grab_layer_verify(bpy.types.Operator):
    bl_idname = 'sculpt.grab_layer_verify'
    bl_label = 'Verifica Grab Layer'
    bl_description = 'Legge impronte di base, layer memorizzati e superficie, senza Commit o Undo'
    bl_options = set()
    stage: EnumProperty(items=[
        ('EMPTY', 'Prima: layer vuoto', ''),
        ('FILLED', 'Prima: layer con deformazione', ''),
        ('AFTER', 'Dopo Grab', ''),
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
        run = _context['run']
        if self.stage in {'EMPTY', 'FILLED'}:
            obj = context.active_object
            modifiers = [m for m in obj.modifiers if m.type == 'MULTIRES']
            if obj.mode != 'SCULPT' or len(modifiers) != 1:
                self.report({'ERROR'}, 'Prima: usa Sculpt Mode con un solo Multires')
                return {'CANCELLED'}
            if not getattr(modifiers[0], 'sculpt_layer_recording', False):
                self.report({'ERROR'}, 'Attiva REC sul layer scelto prima della misura')
                return {'CANCELLED'}
            case = self.stage
            run += 1
            stage = 'BEFORE'
        else:
            if _context['case'] is None:
                self.report({'ERROR'}, 'Premi prima uno dei due pulsanti Prima')
                return {'CANCELLED'}
            stage = self.stage
        label = 'GRAB_%02d_%s_%s' % (run, case, stage)
        result = bpy.ops.sculpt.multires_base_fingerprint(
            label=label, include_layer_payloads=True, include_surface=True)
        if result != {'FINISHED'}:
            self.report({'ERROR'}, 'Impronta non completata; termina pennellate o filtri attivi')
            return {'CANCELLED'}
        _context['case'] = case
        _context['run'] = run
        print('[GRAB_LAYER_VERIFY_V01] label=%s object=%s' % (label, context.active_object.name), flush=True)
        self.report({'INFO'}, 'Impronta registrata: ' + label)
        return {'FINISHED'}

class VIEW3D_PT_grab_layer_verify(bpy.types.Panel):
    bl_label = 'Verifica Grab (temporanea)'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'

    def draw(self, context):
        col = self.layout.column(align=True)
        col.label(text='Prima: Commit, poi riattiva REC')
        col.label(text='I pulsanti registrano soltanto dati')
        col.operator(SCULPT_OT_grab_layer_verify.bl_idname, text='Prima Grab: layer vuoto').stage = 'EMPTY'
        col.operator(SCULPT_OT_grab_layer_verify.bl_idname, text='Prima Grab: layer con deformazione').stage = 'FILLED'
        col.separator()
        for stage, text in [('AFTER', 'Dopo Grab'), ('UNDO', 'Dopo Undo'), ('REDO', 'Dopo Redo'), ('COMMIT', 'Dopo Commit'), ('SWITCH', 'Dopo cambio layer e ritorno'), ('REOPEN', 'Dopo riapertura')]:
            col.operator(SCULPT_OT_grab_layer_verify.bl_idname, text=text).stage = stage

for cls in (SCULPT_OT_grab_layer_verify, VIEW3D_PT_grab_layer_verify):
    old = getattr(bpy.types, cls.__name__, None)
    if old is not None:
        bpy.utils.unregister_class(old)
    bpy.utils.register_class(cls)
print('[GRAB_LAYER_VERIFY_V01] pronto: N > Multires > Verifica Grab; impronte complete solo su richiesta', flush=True)
