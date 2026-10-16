"""Session-only, read-only layer fingerprints for Clay and Clay Strips."""
import json
import bpy
from bpy.props import EnumProperty

_context = {'case': None, 'family': None, 'run': 0}
_starts = {
    'CLAY_EMPTY': ('CLAY', 'EMPTY'),
    'CLAY_FILLED': ('CLAY', 'FILLED'),
    'STRIPS_EMPTY': ('CLAY_STRIPS', 'EMPTY'),
    'STRIPS_FILLED': ('CLAY_STRIPS', 'FILLED'),
}

def active_brush(context):
    settings = getattr(context, 'tool_settings', None)
    sculpt = getattr(settings, 'sculpt', None)
    return getattr(sculpt, 'brush', None)

class SCULPT_OT_clay_layer_verify(bpy.types.Operator):
    bl_idname = 'sculpt.clay_layer_verify'
    bl_label = 'Verifica Clay Layer'
    bl_description = 'Legge impronte; non esegue pennellate, Commit, Undo o salvataggi'
    bl_options = set()
    stage: EnumProperty(items=[
        ('CLAY_EMPTY', 'Prima Clay: layer vuoto', ''),
        ('CLAY_FILLED', 'Prima Clay: layer con dettaglio', ''),
        ('STRIPS_EMPTY', 'Prima Clay Strips: layer vuoto', ''),
        ('STRIPS_FILLED', 'Prima Clay Strips: layer con dettaglio', ''),
        ('AFTER', 'Dopo pennellata', ''),
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
        case, family, run = _context['case'], _context['family'], _context['run']
        brush = active_brush(context)
        if self.stage in _starts:
            obj = context.active_object
            modifiers = [m for m in obj.modifiers if m.type == 'MULTIRES']
            if obj.mode != 'SCULPT' or len(modifiers) != 1:
                self.report({'ERROR'}, 'Usa Sculpt Mode con un solo Multires')
                return {'CANCELLED'}
            if not getattr(modifiers[0], 'sculpt_layer_recording', False):
                self.report({'ERROR'}, 'Riattiva REC prima della misura iniziale')
                return {'CANCELLED'}
            family, case = _starts[self.stage]
            if brush is None or getattr(brush, 'sculpt_brush_type', None) != family:
                self.report({'ERROR'}, 'Seleziona prima il pennello ' + family.replace('_', ' '))
                return {'CANCELLED'}
            run += 1
            stage = 'BEFORE'
        else:
            if case is None:
                self.report({'ERROR'}, 'Premi prima il pulsante Prima del pennello da verificare')
                return {'CANCELLED'}
            stage = self.stage
        label = '%s_%02d_%s_%s' % (family, run, case, stage)
        result = bpy.ops.sculpt.multires_base_fingerprint(
            label=label, include_layer_payloads=True, include_surface=True)
        if result != {'FINISHED'}:
            self.report({'ERROR'}, 'Impronta non completata; termina pennellate o filtri attivi')
            return {'CANCELLED'}
        _context.update(case=case, family=family, run=run)
        info = {'label': label, 'object': context.active_object.name,
                'brush': getattr(brush, 'name', None),
                'brush_type': getattr(brush, 'sculpt_brush_type', None),
                'autosmooth': getattr(brush, 'autosmooth_factor', None)}
        print('[CLAY_LAYER_VERIFY_V01] ' + json.dumps(info), flush=True)
        self.report({'INFO'}, 'Impronta registrata: ' + label)
        return {'FINISHED'}

class VIEW3D_PT_clay_layer_verify(bpy.types.Panel):
    bl_label = 'Verifica Clay (temporanea)'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'

    def draw(self, context):
        col = self.layout.column(align=True)
        col.label(text='Prima: Commit, poi riattiva REC')
        col.label(text='Impronte soltanto: operazioni manuali')
        for stage, text in [
            ('CLAY_EMPTY', 'Prima Clay: layer vuoto'),
            ('CLAY_FILLED', 'Prima Clay: layer con dettaglio'),
            ('STRIPS_EMPTY', 'Prima Clay Strips: layer vuoto'),
            ('STRIPS_FILLED', 'Prima Clay Strips: layer con dettaglio'),
        ]:
            col.operator(SCULPT_OT_clay_layer_verify.bl_idname, text=text).stage = stage
        col.separator()
        for stage, text in [('AFTER', 'Dopo pennellata'), ('UNDO', 'Dopo Undo'),
                            ('REDO', 'Dopo Redo'), ('COMMIT', 'Dopo Commit'),
                            ('SWITCH', 'Dopo cambio layer e ritorno'), ('REOPEN', 'Dopo riapertura')]:
            col.operator(SCULPT_OT_clay_layer_verify.bl_idname, text=text).stage = stage

for cls in (SCULPT_OT_clay_layer_verify, VIEW3D_PT_clay_layer_verify):
    old = getattr(bpy.types, cls.__name__, None)
    if old is not None:
        bpy.utils.unregister_class(old)
    bpy.utils.register_class(cls)
print('[CLAY_LAYER_VERIFY_V01] pronto: N > Multires > Verifica Clay', flush=True)
