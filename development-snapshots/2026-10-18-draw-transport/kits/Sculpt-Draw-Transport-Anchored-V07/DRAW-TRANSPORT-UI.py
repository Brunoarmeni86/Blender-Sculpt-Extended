"""Session UI and dedicated local Draw Transport asset for native V03."""
import bpy


def transport_update(brush, context):
    if brush.sculpt_draw_transport_v01:
        if brush.sculpt_brush_type != 'DRAW':
            brush.sculpt_draw_transport_v01 = False
            return
        brush.stroke_method = 'ANCHORED'
        brush.use_edge_to_edge = False


class SCULPT_OT_draw_transport_brush_v03(bpy.types.Operator):
    bl_idname = 'sculpt.draw_transport_brush_v03'
    bl_label = 'Draw Transport'
    bl_description = 'Crea o attiva il pennello locale Draw Transport, copiando il Draw corrente'
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'SCULPT' and context.tool_settings.sculpt is not None

    def execute(self, context):
        settings = context.tool_settings.sculpt
        original = settings.brush
        brush = next((b for b in bpy.data.brushes
                      if not b.library and b.get('draw_transport_dedicated_v03')
                      and b.sculpt_brush_type == 'DRAW'), None)
        created = False
        if brush is None:
            if original is None or original.sculpt_brush_type != 'DRAW':
                self.report({'ERROR'}, 'Seleziona prima il Draw di cui vuoi copiare texture e impostazioni')
                return {'CANCELLED'}
            brush = original.copy()
            created = True
            brush.name = 'Draw Transport'
            brush.use_fake_user = True
            brush['draw_transport_dedicated_v03'] = True
            brush.asset_mark()
        brush.sculpt_draw_transport_v01 = True
        brush.stroke_method = 'ANCHORED'
        brush.use_edge_to_edge = False
        try:
            result = bpy.ops.sculpt.draw_transport_activate_v03(brush_name=brush.name)
        except Exception as exc:
            if created and settings.brush != brush:
                bpy.data.brushes.remove(brush)
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        if 'FINISHED' not in result or settings.brush != brush:
            if created and settings.brush != brush:
                bpy.data.brushes.remove(brush)
            self.report({'ERROR'}, 'Attivazione non riuscita; il Draw precedente resta disponibile')
            return {'CANCELLED'}
        # The dedicated copy now owns Transport; ordinary Draw keeps its other settings.
        if original and original != brush and not original.library:
            original.sculpt_draw_transport_v01 = False
        self.report({'INFO'}, 'Draw Transport attivo: R rotazione, S scala, SS strength')
        return {'FINISHED'}


class VIEW3D_PT_draw_transport_v01(bpy.types.Panel):
    bl_label = 'Draw Transport V07'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'

    @classmethod
    def poll(cls, context):
        return context.mode == 'SCULPT' and context.tool_settings.sculpt is not None

    def draw(self, context):
        layout = self.layout
        layout.operator('sculpt.draw_transport_brush_v03', text='Attiva Draw Transport', icon='BRUSH_DATA')
        brush = context.tool_settings.sculpt.brush
        if brush is None or brush.sculpt_brush_type != 'DRAW':
            layout.label(text='Prima creazione: seleziona un Draw')
            return
        layout.prop(brush, 'sculpt_draw_transport_v01', text='Trasporto dopo Anchored')
        layout.label(text='Rilascia, poi muovi: trasporta')
        layout.label(text='R: ruota | S: scala | SS: strength')
        layout.label(text='Clic: accetta controllo, poi conferma')
        layout.label(text='Esc / destro: annulla lo stroke')
        layout.separator()
        layout.label(text='Multires massimo; simmetrie OFF')
        layout.label(text='REC attivo se usi Sculpt Layers')


def draw_transport_header(self, context):
    if context.mode == 'SCULPT':
        self.layout.operator('sculpt.draw_transport_brush_v03', text='Draw Transport', icon='BRUSH_DATA')


if bpy.ops.sculpt.brush_stroke.get_rna_type().properties.get('draw_transport') is None:
    raise RuntimeError('Draw Transport V07: manca la patch nativa')
try:
    bpy.ops.sculpt.draw_transport_activate_v03.get_rna_type()
except Exception:
    raise RuntimeError('Draw Transport V07: ricompila prima la nuova versione')

# Reload cleanly in the same session without leaving duplicate header callbacks.
old_header = bpy.app.driver_namespace.pop('draw_transport_header_v03', None)
if old_header is not None:
    bpy.types.VIEW3D_HT_tool_header.remove(old_header)
for name in ('VIEW3D_PT_draw_transport_v01', 'SCULPT_OT_draw_transport_brush_v03'):
    old = getattr(bpy.types, name, None)
    if old is not None:
        bpy.utils.unregister_class(old)
if not hasattr(bpy.types.Brush, 'sculpt_draw_transport_v01'):
    bpy.types.Brush.sculpt_draw_transport_v01 = bpy.props.BoolProperty(
        name='Draw Transport Anchored', default=False, update=transport_update,
        description='Mantiene lo stroke Draw Anchored riposizionabile dopo il rilascio')
bpy.utils.register_class(SCULPT_OT_draw_transport_brush_v03)
bpy.utils.register_class(VIEW3D_PT_draw_transport_v01)
bpy.types.VIEW3D_HT_tool_header.append(draw_transport_header)
bpy.app.driver_namespace['draw_transport_header_v03'] = draw_transport_header
print('[DRAW_TRANSPORT_V07] UI pronta: pulsante Draw Transport nell intestazione Sculpt; F3 > Draw Transport', flush=True)
