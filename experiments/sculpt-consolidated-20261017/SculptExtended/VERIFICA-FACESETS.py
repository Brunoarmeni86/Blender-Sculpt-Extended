"""Face Set panel with explicit viewport picking. Session-only; native operators own Undo."""
import bpy
import os
from bpy.props import EnumProperty


def inside_region(region, x, y):
    return (region.width > 1 and region.height > 1 and
            region.x <= x < region.x + region.width and
            region.y <= y < region.y + region.height)


def pick_window_region(area, x, y):
    # WINDOW can extend beneath an overlapping N sidebar. Reject UI/header hits.
    for region in area.regions:
        if region.type != 'WINDOW' and inside_region(region, x, y):
            return None
    return next((region for region in area.regions
                 if region.type == 'WINDOW' and inside_region(region, x, y)), None)


PICK_ACTIONS = {
    'ISOLATE': ('sculpt', 'face_set_change_visibility', {'mode': 'TOGGLE'}),
    'HIDE': ('sculpt', 'face_set_change_visibility', {'mode': 'HIDE_ACTIVE'}),
    'MASK': ('sculpt', 'multires_face_sets_hr_mask', {'mode': 'REPLACE'}),
    'ADD': ('sculpt', 'multires_face_sets_hr_mask', {'mode': 'ADD'}),
    'REMOVE': ('sculpt', 'multires_face_sets_hr_mask', {'mode': 'REMOVE'}),
    'OTHERS': ('sculpt', 'multires_face_sets_hr_mask', {'mode': 'OTHERS'}),
    'AUDIT': ('sculpt', 'multires_face_sets_hr_state', {}),
    'BOX': ('sculpt', 'face_set_box_gesture', {}),
    'LASSO': ('sculpt', 'face_set_lasso_gesture', {}),
    'LINE': ('sculpt', 'face_set_line_gesture', {}),
    'POLYLINE': ('sculpt', 'face_set_polyline_gesture', {}),
    'EXTRACT_FACE_SET': ('sculpt', 'multires_hr_extract_v05', {'source': 'FACE_SET'}),
}


class SCULPT_OT_facesets_hr_panel_pick_v03(bpy.types.Operator):
    bl_idname = 'sculpt.facesets_hr_panel_pick_v03'
    bl_label = 'Scegli Face Set sulla superficie'
    bl_description = 'Sposta il cursore sulla mesh e clicca; Esc annulla senza modifiche'
    bl_options = {'INTERNAL'}
    action: EnumProperty(items=[(key, key, '') for key in PICK_ACTIONS], default='ISOLATE')

    @classmethod
    def poll(cls, context):
        return bool(context.area and context.area.type == 'VIEW_3D' and
                    context.active_object and context.active_object.type == 'MESH' and
                    context.mode == 'SCULPT')

    def invoke(self, context, event):
        self._area_pointer = context.area.as_pointer()
        self._object_pointer = context.active_object.as_pointer()
        self._window = context.window
        self._workspace = context.workspace
        self._window.cursor_modal_set('CROSSHAIR')
        self._workspace.status_text_set('Face Sets HR: clicca sulla mesh; Esc o tasto destro annulla')
        context.window_manager.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def finish_picker(self):
        window = getattr(self, '_window', None)
        if window is not None:
            try:
                window.cursor_modal_restore()
            except ReferenceError:
                pass
            self._window = None
        workspace = getattr(self, '_workspace', None)
        if workspace is not None:
            try:
                workspace.status_text_set(None)
            except ReferenceError:
                pass
            self._workspace = None

    def cancel(self, context):
        self.finish_picker()

    def modal(self, context, event):
        if event.type in {'ESC', 'RIGHTMOUSE'}:
            self.finish_picker()
            return {'CANCELLED'}
        window = self._window
        ob = context.active_object
        if (window is None or context.window != window or ob is None or
                ob.as_pointer() != self._object_pointer or ob.mode != 'SCULPT'):
            self.finish_picker()
            return {'CANCELLED'}
        area = next((a for a in window.screen.areas if a.as_pointer() == self._area_pointer), None)
        if area is None or area.type != 'VIEW_3D':
            self.finish_picker()
            return {'CANCELLED'}
        if event.type == 'LEFTMOUSE' and event.value == 'PRESS':
            region = pick_window_region(area, event.mouse_x, event.mouse_y)
            if region is None:
                self.report({'INFO'}, 'Clicca nella vista 3D, fuori dal pannello')
                return {'RUNNING_MODAL'}
            namespace, name, kwargs = PICK_ACTIONS[self.action]
            # No wrapper Undo step: native child operator receives True explicitly.
            # WM recomputes event.mval relative to this WINDOW region on invocation.
            self.finish_picker()
            try:
                with context.temp_override(window=window, area=area, region=region,
                                           active_object=ob, object=ob):
                    result = getattr(getattr(bpy.ops, namespace), name)(
                        'INVOKE_DEFAULT', True, **kwargs)
            except (RuntimeError, AttributeError) as exc:
                self.report({'ERROR'}, str(exc))
                return {'CANCELLED'}
            if 'CANCELLED' in result:
                return {'CANCELLED'}
            # A gesture may now own its own modal handler; never forward our click
            # to the sculpt brush and never keep a second modal picker running.
            return {'FINISHED'}
        if event.type in {'MIDDLEMOUSE', 'WHEELUPMOUSE', 'WHEELDOWNMOUSE',
                          'TRACKPADPAN', 'TRACKPADZOOM', 'NDOF_MOTION'}:
            return {'PASS_THROUGH'}
        return {'RUNNING_MODAL'}


class SCULPT_PT_facesets_hr_tools_v02(bpy.types.Panel):
    bl_label = 'Face Sets HR'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'

    @classmethod
    def poll(cls, context):
        return bool(context.active_object and context.active_object.type == 'MESH' and
                    context.mode == 'SCULPT')

    def draw(self, context):
        layout = self.layout
        layout.operator_context = 'INVOKE_DEFAULT'
        layout.label(text='Draw Face Sets: REC indipendente')
        box = layout.box()
        box.label(text='Visibilita del gruppo')
        for action, text in [('ISOLATE','Isola / Mostra tutto'), ('HIDE','Nascondi gruppo')]:
            box.operator('sculpt.facesets_hr_panel_pick_v03', text=text).action = action
        box.operator_context = 'EXEC_REGION_WIN'
        box.operator('paint.hide_show_all', text='Mostra tutto').action = 'SHOW'
        box = layout.box()
        box.label(text='Maschera dal gruppo')
        for action, text in [('MASK','Maschera gruppo'),('ADD','Aggiungi alla maschera'),
                             ('REMOVE','Rimuovi dalla maschera'),('OTHERS','Maschera gli altri')]:
            box.operator('sculpt.facesets_hr_panel_pick_v03', text=text).action = action
        layout.label(text='Pulsante, poi clic sulla superficie', icon='INFO')
        if os.environ.get('BLENDER_MULTIRES_FACESETS_GESTURES_V03') == '1':
            box = layout.box()
            box.label(text='Crea un gruppo con un gesto')
            for action, text in [('BOX','Rettangolo'),('LASSO','Lasso'),
                                 ('LINE','Linea'),('POLYLINE','Polilinea')]:
                box.operator('sculpt.facesets_hr_panel_pick_v03', text=text).action = action
        if os.environ.get('BLENDER_MULTIRES_HR_EXTRACT_V05') == '1':
            box = layout.box()
            box.label(text='Estrai in un nuovo oggetto')
            box.operator_context = 'INVOKE_REGION_WIN'
            box.operator('sculpt.multires_hr_extract_v05', text='Mask Extract HR').source = 'MASK'
            box.operator_context = 'INVOKE_DEFAULT'
            box.operator('sculpt.facesets_hr_panel_pick_v03', text='Estrai Face Set HR').action = 'EXTRACT_FACE_SET'
            box.label(text='Risoluzione = livello Sculpt corrente')
        box = layout.box()
        box.label(text='Verifica')
        box.operator('sculpt.facesets_hr_panel_pick_v03', text='Leggi gruppo sotto cursore').action = 'AUDIT'
        box.operator_context = 'EXEC_DEFAULT'
        for label, text in [('BEFORE','Impronta prima'),('AFTER','Impronta dopo'),
                             ('UNDO','Impronta dopo Undo'),('REOPEN','Impronta dopo riapertura')]:
            box.operator('sculpt.multires_face_sets_audit', text=text).label = 'FS_TOOLS_' + label
        box = layout.box()
        box.label(text='Sostituisce i confini dettagliati', icon='ERROR')
        box.operator_context = 'EXEC_DEFAULT'
        box.operator('sculpt.multires_face_sets_from_base', text='Ripristina gruppi dalla base')


if os.environ.get('BLENDER_MULTIRES_FACESETS_VISIBILITY_V02') != '1':
    raise RuntimeError('Usa il launcher Face Sets HR Tools V02 o Gestures V03')
for name in ('multires_face_sets_audit', 'multires_face_sets_hr_mask', 'multires_face_sets_hr_state'):
    getattr(bpy.ops.sculpt, name).get_rna_type()
if os.environ.get('BLENDER_MULTIRES_HR_EXTRACT_V05') == '1':
    bpy.ops.sculpt.multires_hr_extract_v05.get_rna_type()
for cls in (SCULPT_PT_facesets_hr_tools_v02, SCULPT_OT_facesets_hr_panel_pick_v03):
    old = getattr(bpy.types, cls.__name__, None)
    if old is not None:
        bpy.utils.unregister_class(old)
old = getattr(bpy.types, 'SCULPT_PT_facesets_hr_v01', None)
if old is not None:
    bpy.utils.unregister_class(old)
bpy.utils.register_class(SCULPT_OT_facesets_hr_panel_pick_v03)
bpy.utils.register_class(SCULPT_PT_facesets_hr_tools_v02)
print('[FACESETS_HR_PANEL_V05] explicit viewport picker registered; native operators own Undo', flush=True)
