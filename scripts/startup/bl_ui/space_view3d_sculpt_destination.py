# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit UI for the existing layer/base routing. No new displacement solver."""
import json
import math
import bpy

KEY = '_sculpt_destination_visibility_v01'


def modifier(context):
    obj = context.object
    if not obj or obj.type != 'MESH':
        return None
    items = [m for m in obj.modifiers if m.type == 'MULTIRES' and m.show_viewport]
    return items[0] if len(items) == 1 else None


def active(m):
    index = m.active_sculpt_layer_index
    return m.sculpt_layers[index] if 0 <= index < len(m.sculpt_layers) else None


def geometric_target(m):
    if m.use_sculpt_base_mesh:
        return 'CAGE', 'Base cage (Sculpt Base Mesh)'
    if not any(layer.visible for layer in m.sculpt_layers):
        return 'BASE', 'Base Multires'
    layer = active(m)
    if layer is None or not layer.visible or not math.isfinite(layer.strength) or abs(layer.strength) < 1e-6:
        return 'BLOCKED', 'Layer non modificabile'
    return 'LAYER', 'Layer: ' + layer.name


def status(context, m):
    # Color/mask/Face Sets brushes write distinct domains, never geometric layers.
    sculpt = context.tool_settings.sculpt
    brush = sculpt.brush if sculpt else None
    kind = getattr(brush, 'sculpt_brush_type', '')
    if kind in {'PAINT', 'BLUR', 'SMEAR'}:
        return 'COLOR', 'Polypaint: ' + (m.polypaint_attribute or 'attributo colore attivo')
    if kind == 'MASK':
        return 'MASK', 'Maschera Sculpt'
    if kind == 'DRAW_FACE_SETS':
        return 'FACESETS', 'Face Sets'
    return geometric_target(m)


def read_saved(obj, m):
    raw = obj.get(KEY)
    if raw is None:
        return None
    data = json.loads(raw)
    if data.get('modifier') != m.name or data.get('names') != [l.name for l in m.sculpt_layers]:
        raise ValueError('La lista dei layer e cambiata: ripristina la visibilita manualmente e rimuovi la memoria con il pulsante dedicato')
    values = data.get('visible')
    if not isinstance(values, list) or len(values) != len(m.sculpt_layers) or any(type(v) is not bool for v in values):
        raise ValueError('Memoria della visibilita non valida')
    return data


def ready(context):
    obj = context.object
    m = modifier(context)
    return bool(obj and obj.mode == 'SCULPT' and m and not obj.library and not obj.data.library)


def flush():
    result = bpy.ops.sculpt.multires_layer_commit('EXEC_DEFAULT')
    if result != {'FINISHED'}:
        raise RuntimeError('Impossibile confermare le modifiche in sospeso; destinazione invariata')


def apply_visibility(context, m, values):
    for layer, value in zip(m.sculpt_layers, values):
        if layer.visible != value:
            layer.visible = value
            context.view_layer.update()
            if layer.visible != value:
                raise RuntimeError('Cambio visibilita rifiutato dal percorso nativo')


class SCULPT_OT_destination_v01(bpy.types.Operator):
    bl_idname = 'sculpt.destination_v01'
    bl_label = 'Destinazione Sculpt'
    bl_description = 'Base: nasconde i layer salvandone la visibilita. Layer: ripristina la visibilita e mostra quello selezionato. Non esegue bake'
    bl_options = {'REGISTER', 'UNDO'}

    target: bpy.props.EnumProperty(items=[('BASE', 'Base Multires', ''), ('LAYER', 'Layer attivo', '')])

    @classmethod
    def poll(cls, context):
        return ready(context)

    def execute(self, context):
        obj, m = context.object, modifier(context)
        if m.use_sculpt_base_mesh:
            self.report({'ERROR'}, 'Disattiva Sculpt Base Mesh per scegliere tra superficie Multires e layer')
            return {'CANCELLED'}
        layers = list(m.sculpt_layers)
        layer = active(m)
        before = [l.visible for l in layers]
        previous_raw = obj.get(KEY)
        try:
            saved = read_saved(obj, m)
            if self.target == 'BASE':
                if not any(before):
                    return {'FINISHED'}
                # Never overwrite the first visibility snapshot after manual edits.
                if saved is not None:
                    raise ValueError('Ripristina prima la visibilita salvata, oppure rimuovi la memoria')
                values = [False] * len(layers)
                new_raw = json.dumps(dict(modifier=m.name, names=[l.name for l in layers], visible=before))
            else:
                if layer is None:
                    raise ValueError('Crea e seleziona un layer prima di attivare questa destinazione')
                if not math.isfinite(layer.strength) or abs(layer.strength) < 1e-6:
                    raise ValueError('Il layer selezionato ha intensita zero o non valida')
                values = list(saved['visible']) if saved else list(before)
                values[m.active_sculpt_layer_index] = True
                new_raw = None
            if layers:
                flush()
                context.view_layer.update()
            apply_visibility(context, m, values)
            if new_raw is None:
                if KEY in obj:
                    del obj[KEY]
            else:
                obj[KEY] = new_raw
            for area in context.screen.areas:
                area.tag_redraw()
            return {'FINISHED'}
        except Exception as exc:
            # Native setters remain the sole visibility writers. No raw RNA or
            # grid mutation is used, including rollback after an exceptional failure.
            try:
                apply_visibility(context, m, before)
                if previous_raw is None:
                    if KEY in obj:
                        del obj[KEY]
                else:
                    obj[KEY] = previous_raw
            except Exception:
                self.report({'ERROR'}, 'Ripristino visibilita incompleto: usare Undo e controllare i layer')
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}


class SCULPT_OT_destination_forget_v01(bpy.types.Operator):
    bl_idname = 'sculpt.destination_forget_v01'
    bl_label = 'Rimuovi memoria visibilita'
    bl_description = 'Rimuove soltanto la memoria del selettore; non cambia visibilita, geometria o layer'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return ready(context) and KEY in context.object

    def execute(self, context):
        del context.object[KEY]
        return {'FINISHED'}


def draw_controls(layout, context, m):
    target, text = geometric_target(m)
    row = layout.row(align=True)
    row.enabled = ready(context) and target != 'CAGE'
    row.operator('sculpt.destination_v01', text='Base', depress=target == 'BASE').target = 'BASE'
    layer_button = row.row(align=True)
    layer = active(m)
    layer_button.enabled = bool(layer and math.isfinite(layer.strength) and abs(layer.strength) >= 1e-6)
    layer_button.operator('sculpt.destination_v01', text='Layer attivo', depress=target == 'LAYER').target = 'LAYER'
    return target, text


def draw_header(self, context):
    if not context.object or context.object.mode != 'SCULPT':
        return
    m = modifier(context)
    if m is None:
        return
    kind, text = status(context, m)
    row = self.layout.row(align=True)
    row.alert = kind == 'BLOCKED'
    row.label(text=text, icon='ERROR' if kind == 'BLOCKED' else 'INFO')
    if kind not in {'COLOR', 'MASK', 'FACESETS'}:
        draw_controls(row, context, m)


class VIEW3D_PT_sculpt_destination_v01(bpy.types.Panel):
    bl_label = 'Sculpt Destination'
    bl_order = 40
    bl_parent_id = 'VIEW3D_PT_sculpt_multires_sidebar'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return ready(context)

    def draw(self, context):
        layout = self.layout
        m = modifier(context)
        kind, text = status(context, m)
        layout.label(text=text, icon='ERROR' if kind == 'BLOCKED' else 'INFO')
        draw_controls(layout, context, m)
        layout.label(text='Base: layer temporaneamente nascosti')
        layout.label(text='Layer: ripristina la visibilita salvata')
        if KEY in context.object:
            layout.label(text='Visibilita dei layer memorizzata', icon='INFO')
            layout.operator('sculpt.destination_forget_v01')


classes = (SCULPT_OT_destination_v01, SCULPT_OT_destination_forget_v01, VIEW3D_PT_sculpt_destination_v01)


def register_header():
    unregister_header()
    bpy.types.VIEW3D_HT_header.append(draw_header)


def unregister_header():
    try:
        bpy.types.VIEW3D_HT_header.remove(draw_header)
    except ValueError:
        pass
