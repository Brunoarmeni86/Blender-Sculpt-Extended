"""Session UI only. No handlers, timers, scene writes, or automatic fingerprinting."""
import bpy
import runpy
from pathlib import Path

runpy.run_path(str(Path(__file__).with_name('LOAD-SIDEBAR.py')))

class VIEW3D_PT_multires_undo_audit(bpy.types.Panel):
    bl_label = 'Diagnosi Undo / Base'
    bl_idname = 'VIEW3D_PT_multires_undo_audit'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.type == 'MESH' and any(m.type == 'MULTIRES' for m in obj.modifiers)

    def draw(self, context):
        layout = self.layout
        layout.label(text='Lettura completa: base, layer, CCG')
        layout.label(text='Rilascia la pennellata prima del controllo')
        for label, title in (
            ('baseline', '1. Prima della pennellata'),
            ('before-undo', '2. Prima di Undo'),
            ('after-undo', '3. Dopo Undo'),
            ('after-redo', '4. Dopo Redo'),
            ('after-flush', '5. Dopo Commit (facoltativo)'),
        ):
            op = layout.operator('sculpt.multires_base_fingerprint', text=title)
            op.label = label
            op.include_layer_payloads = True
            op.include_surface = True
        layout.label(text='I controlli non aggiungono passi Undo')

if hasattr(bpy.types, 'VIEW3D_PT_multires_undo_audit'):
    bpy.utils.unregister_class(bpy.types.VIEW3D_PT_multires_undo_audit)
bpy.utils.register_class(VIEW3D_PT_multires_undo_audit)
print('[MULTIRES_AUDIT_UI] N > Multires > Diagnosi Undo / Base', flush=True)
