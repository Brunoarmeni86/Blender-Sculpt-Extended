# SPDX-License-Identifier: GPL-2.0-or-later
# Internal UI for the native Multires Polypaint bake operator.
import bpy
import os
import math
from array import array
from bpy.props import StringProperty, IntProperty, EnumProperty, PointerProperty, BoolProperty
from bpy_extras.io_utils import ExportHelper


def active_mesh(context):
    ob = context.active_object
    return ob if ob and ob.type == 'MESH' else None


def native_available():
    try:
        bpy.ops.sculpt.multires_polypaint_uv_bake_v01.get_rna_type()
        return True
    except (AttributeError, RuntimeError):
        return False


class MPBI_Settings(bpy.types.PropertyGroup):
    attribute: StringProperty(name='Color Attribute')
    uv_map: StringProperty(name='UV Map')
    resolution: EnumProperty(name='Risoluzione', items=[(str(n), str(n), '') for n in (512, 1024, 2048, 4096)], default='2048')
    samples: EnumProperty(name='Antialiasing', items=[('1', 'Off (1 sample)', ''), ('2', '2 x 2 (4 samples)', '')], default='2')
    bake_mode: EnumProperty(name='Tile', items=[('SINGLE', 'Singolo', ''), ('ALL', 'Tutti i tile UV', '')], default='SINGLE')
    save_series: BoolProperty(name='Salva PNG progressivamente', default=False)
    release_saved: BoolProperty(name='Rimuovi immagini dopo il salvataggio', default=False)
    output_dir: StringProperty(name='Cartella', subtype='DIR_PATH')
    output_name: StringProperty(name='Nome file', default='Polypaint')
    tile: IntProperty(name='Tile UDIM', default=1001, min=1001, max=2000)
    margin: IntProperty(name='Margine (px)', default=8, min=0, max=64)
    image: PointerProperty(name='Risultato', type=bpy.types.Image)


class MPBI_OT_bake(bpy.types.Operator):
    bl_idname = 'multires_polypaint_internal.bake'
    bl_label = 'Bake colore su UV'
    bl_description = 'Crea una nuova immagine dal master Polypaint Multires'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return bool(active_mesh(context) and context.mode == 'OBJECT' and native_available())

    def execute(self, context):
        ob = active_mesh(context)
        cfg = context.scene.multires_polypaint_bake_internal
        if not ob.data.uv_layers:
            self.report({'ERROR'}, 'La mesh non ha una mappa UV')
            return {'CANCELLED'}
        attr = cfg.attribute
        if not attr:
            mmd = next((m for m in ob.modifiers if m.type == 'MULTIRES'), None)
            attr = getattr(mmd, 'sculpt_color_attribute', '') if mmd else ''
            if not attr and ob.data.color_attributes.active_color:
                attr = ob.data.color_attributes.active_color.name
        uv = cfg.uv_map or ob.data.uv_layers.active.name
        if uv not in ob.data.uv_layers:
            self.report({'ERROR'}, 'La UV Map selezionata non esiste su questo oggetto')
            return {'CANCELLED'}
        before = {im.as_pointer() for im in bpy.data.images}
        try:
            kwargs = dict(attribute=attr, uv_map=uv, resolution=int(cfg.resolution), tile=cfg.tile, margin=cfg.margin)
            if native_has_aa():
                kwargs['samples'] = int(cfg.samples)
            result = bpy.ops.sculpt.multires_polypaint_uv_bake_v01('EXEC_DEFAULT', **kwargs)
        except RuntimeError as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        if 'FINISHED' not in result:
            return {'CANCELLED'}
        created = [im for im in bpy.data.images if im.as_pointer() not in before]
        if created:
            cfg.image = created[-1]
            for area in context.screen.areas:
                if area.type == 'IMAGE_EDITOR':
                    area.spaces.active.image = cfg.image
            self.report({'INFO'}, f'Bake completato: {cfg.image.name}')
        return {'FINISHED'}


def _clip_polygon(poly, axis, bound, keep_above):
    if not poly:
        return []
    result = []
    prev = poly[-1]
    prev_in = prev[axis] >= bound if keep_above else prev[axis] <= bound
    for cur in poly:
        cur_in = cur[axis] >= bound if keep_above else cur[axis] <= bound
        if cur_in != prev_in:
            t = (bound-prev[axis])/(cur[axis]-prev[axis])
            result.append((prev[0]+t*(cur[0]-prev[0]), prev[1]+t*(cur[1]-prev[1])))
        if cur_in:
            result.append(cur)
        prev, prev_in = cur, cur_in
    return result


def occupied_udim_tiles(polygons):
    tiles = set()
    for poly in polygons:
        if len(poly) < 3:
            continue
        if any(not math.isfinite(x) or not math.isfinite(y) for x, y in poly):
            raise ValueError('UV non finite')
        if any(x < 0 or x > 10 or y < 0 or y > 100 for x, y in poly):
            raise ValueError('UV fuori dall’intervallo UDIM supportato: U 0..10, V 0..100')
        x0, x1 = max(0, math.floor(min(x for x, y in poly))), min(9, math.ceil(max(x for x, y in poly))-1)
        y0, y1 = max(0, math.floor(min(y for x, y in poly))), min(99, math.ceil(max(y for x, y in poly))-1)
        for y in range(y0, y1+1):
            for x in range(x0, x1+1):
                clipped = poly
                for axis, bound, above in ((0,x,True),(0,x+1,False),(1,y,True),(1,y+1,False)):
                    clipped = _clip_polygon(clipped, axis, bound, above)
                area2 = abs(sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(clipped, clipped[1:]+clipped[:1])))
                if area2 > 1e-12:
                    tiles.add(1001+x+10*y)
    return sorted(tiles)


_series_active = False
_series_cancel = False


def native_has_aa():
    return 'samples' in bpy.ops.sculpt.multires_polypaint_uv_bake_v01.get_rna_type().properties


class MPBI_OT_cancel_series(bpy.types.Operator):
    bl_idname = 'multires_polypaint_internal.cancel_series'
    bl_label = 'Ferma dopo il tile corrente'

    def execute(self, context):
        global _series_cancel
        _series_cancel = True
        return {'FINISHED'}


class MPBI_OT_series(bpy.types.Operator):
    bl_idname = 'multires_polypaint_internal.series'
    bl_label = 'Bake serie UDIM'
    bl_description = 'Elabora in ordine i tile UV occupati, uno alla volta; ESC interrompe fra i tile'
    # Each native bake has its own Undo entry; saved files are external outputs.
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return bool(not _series_active and active_mesh(context) and context.mode == 'OBJECT' and native_available())

    def execute(self, context):
        global _series_active, _series_cancel
        ob = active_mesh(context)
        cfg = context.scene.multires_polypaint_bake_internal
        uv_name = cfg.uv_map or (ob.data.uv_layers.active.name if ob.data.uv_layers.active else '')
        uv_attr = ob.data.attributes.get(uv_name)
        if uv_attr is None or uv_attr.domain != 'CORNER' or uv_attr.data_type != 'FLOAT2':
            self.report({'ERROR'}, 'Seleziona una UV Map valida')
            return {'CANCELLED'}
        values = array('f', [0.0]) * (2 * len(uv_attr.data))
        uv_attr.data.foreach_get('vector', values)
        polygons = [[(values[2*i], values[2*i+1]) for i in p.loop_indices] for p in ob.data.polygons]
        try:
            self._tiles = occupied_udim_tiles(polygons)
        except ValueError as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        if not self._tiles:
            self.report({'ERROR'}, 'Nessun tile UV occupato')
            return {'CANCELLED'}
        attr = cfg.attribute
        if not attr:
            mmd = next((m for m in ob.modifiers if m.type == 'MULTIRES'), None)
            attr = getattr(mmd, 'sculpt_color_attribute', '') if mmd else ''
            if not attr and ob.data.color_attributes.active_color:
                attr = ob.data.color_attributes.active_color.name
        self._kwargs = dict(attribute=attr, uv_map=uv_name, resolution=int(cfg.resolution), margin=cfg.margin)
        if native_has_aa():
            self._kwargs['samples'] = int(cfg.samples)
        self._paths = {}
        if cfg.save_series:
            folder = os.path.abspath(bpy.path.abspath(cfg.output_dir)) if cfg.output_dir else ''
            if not folder or not os.path.isdir(folder):
                self.report({'ERROR'}, 'Scegli una cartella di output esistente')
                return {'CANCELLED'}
            basename = bpy.path.clean_name(cfg.output_name.strip() or ob.name)
            for tile in self._tiles:
                path = os.path.join(folder, f'{basename}.{tile}.png')
                if os.path.exists(path):
                    self.report({'ERROR'}, 'File già presente: ' + path + '. Cambia cartella o nome.')
                    return {'CANCELLED'}
                self._paths[tile] = path
        self._release = bool(cfg.save_series and cfg.release_saved)
        self._object = ob.as_pointer()
        self._mesh = ob.data.as_pointer()
        self._scene = context.scene.as_pointer()
        self._index = self._completed = self._skipped = 0
        self._timer = context.window_manager.event_timer_add(0.15, window=context.window)
        context.window_manager.progress_begin(0, len(self._tiles))
        _series_active, _series_cancel = True, False
        context.window_manager.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def finish(self, context, message, error=False):
        global _series_active, _series_cancel
        if self._timer:
            context.window_manager.event_timer_remove(self._timer)
            self._timer = None
        context.window_manager.progress_end()
        context.workspace.status_text_set(None)
        _series_active, _series_cancel = False, False
        self.report({'WARNING'} if error else {'INFO'}, message + f' Completati: {self._completed}; saltati: {self._skipped}.')
        return {'FINISHED'} if self._completed else {'CANCELLED'}

    def modal(self, context, event):
        if event.type == 'ESC' or _series_cancel:
            return self.finish(context, 'Serie interrotta: risultati completati conservati.')
        if event.type != 'TIMER' or event.timer != self._timer:
            return {'RUNNING_MODAL'}
        ob = active_mesh(context)
        if not ob or ob.as_pointer() != self._object or ob.data.as_pointer() != self._mesh or context.scene.as_pointer() != self._scene or context.mode != 'OBJECT':
            return self.finish(context, 'Oggetto o contesto cambiato.', True)
        if self._index == len(self._tiles):
            return self.finish(context, 'Serie UDIM completata.')
        tile = self._tiles[self._index]
        context.workspace.status_text_set(f'Polypaint: tile {tile} ({self._index+1}/{len(self._tiles)}). ESC fra i tile.')
        before = {im.as_pointer() for im in bpy.data.images}
        try:
            result = bpy.ops.sculpt.multires_polypaint_uv_bake_v01('EXEC_DEFAULT', tile=tile, **self._kwargs)
        except RuntimeError as exc:
            if 'No UV coverage in selected tile' in str(exc):
                self._skipped += 1
                self._index += 1
                context.window_manager.progress_update(self._index)
                return {'RUNNING_MODAL'}
            return self.finish(context, str(exc), True)
        if 'FINISHED' not in result:
            return self.finish(context, f'Bake tile {tile} non completato.', True)
        images = [im for im in bpy.data.images if im.as_pointer() not in before]
        if len(images) != 1:
            return self.finish(context, 'Risultato immagine non univoco.', True)
        image = images[0]
        cfg = context.scene.multires_polypaint_bake_internal
        cfg.image = image
        self._completed += 1
        if tile in self._paths:
            path = self._paths[tile]
            # Reserve exclusively so even a file created after preflight is not overwritten.
            try:
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
                os.close(fd)
            except OSError as exc:
                return self.finish(context, f'Immagine conservata, salvataggio bloccato: {exc}', True)
            try:
                image.filepath_raw = path
                image.file_format = 'PNG'
                image.save()
            except (RuntimeError, OSError) as exc:
                # Remove only the zero-byte placeholder, never another completed output.
                if os.path.exists(path) and os.path.getsize(path) == 0:
                    os.unlink(path)
                return self.finish(context, f'Immagine conservata: salvataggio fallito: {exc}', True)
            if self._release:
                cfg.image = None
                bpy.data.images.remove(image)
        if cfg.image:
            for area in context.screen.areas:
                if area.type == 'IMAGE_EDITOR':
                    area.spaces.active.image = cfg.image
        self._index += 1
        context.window_manager.progress_update(self._index)
        return {'RUNNING_MODAL'}

    def cancel(self, context):
        if getattr(self, '_timer', None):
            self.finish(context, 'Serie interrotta.')


class MPBI_OT_show(bpy.types.Operator):
    bl_idname = 'multires_polypaint_internal.show'
    bl_label = 'Mostra immagine'

    def execute(self, context):
        image = context.scene.multires_polypaint_bake_internal.image
        if not image:
            return {'CANCELLED'}
        area = next((a for a in context.screen.areas if a.type == 'IMAGE_EDITOR'), None)
        if area:
            area.spaces.active.image = image
        else:
            # User explicitly requested viewing the result: open a separate editor.
            try:
                before = {w.as_pointer() for w in context.window_manager.windows}
                bpy.ops.screen.area_dupli('EXEC_DEFAULT')
                win = next((w for w in context.window_manager.windows if w.as_pointer() not in before), None)
                if win is None:
                    self.report({'INFO'}, 'Apri un Image Editor e seleziona ' + image.name)
                    return {'FINISHED'}
                area = win.screen.areas[0]
                area.type = 'IMAGE_EDITOR'
                area.spaces.active.image = image
            except RuntimeError:
                self.report({'INFO'}, 'Apri un Image Editor e seleziona ' + image.name)
        return {'FINISHED'}


class MPBI_OT_save(bpy.types.Operator, ExportHelper):
    bl_idname = 'multires_polypaint_internal.save'
    bl_label = 'Salva PNG'
    filename_ext = '.png'
    filter_glob: StringProperty(default='*.png', options={'HIDDEN'})

    def invoke(self, context, event):
        image = context.scene.multires_polypaint_bake_internal.image
        if not image:
            return {'CANCELLED'}
        self.filepath = bpy.path.clean_name(image.name) + '.png'
        return ExportHelper.invoke(self, context, event)

    def execute(self, context):
        image = context.scene.multires_polypaint_bake_internal.image
        if not image:
            self.report({'ERROR'}, 'Nessuna immagine selezionata')
            return {'CANCELLED'}
        old_path, old_format = image.filepath_raw, image.file_format
        try:
            image.filepath_raw = self.filepath
            image.file_format = 'PNG'
            image.save()
        except (RuntimeError, OSError) as exc:
            image.filepath_raw, image.file_format = old_path, old_format
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        self.report({'INFO'}, 'PNG salvato: ' + self.filepath)
        return {'FINISHED'}


def draw_bake_controls(layout, context):
    cfg = context.scene.multires_polypaint_bake_internal
    mesh = context.active_object.data
    if not native_available():
        layout.label(text='Build Polypaint UV Bake richiesta', icon='ERROR')
        return
    if context.mode != 'OBJECT':
        layout.label(text='Passa in Object Mode per il bake', icon='INFO')
    layout.use_property_split = True
    layout.prop_search(cfg, 'attribute', mesh, 'color_attributes')
    layout.prop_search(cfg, 'uv_map', mesh, 'uv_layers')
    layout.prop(cfg, 'resolution')
    aa = layout.row()
    aa.enabled = native_has_aa()
    aa.prop(cfg, 'samples')
    if not native_has_aa():
        layout.label(text='Antialiasing: richiede bake nativo V02', icon='INFO')
    layout.prop(cfg, 'bake_mode')
    if cfg.bake_mode == 'SINGLE':
        layout.prop(cfg, 'tile')
    layout.prop(cfg, 'margin')
    if cfg.bake_mode == 'ALL':
        layout.prop(cfg, 'save_series')
        if cfg.save_series:
            layout.prop(cfg, 'output_dir')
            layout.prop(cfg, 'output_name')
            layout.prop(cfg, 'release_saved')
        row = layout.row()
        row.enabled = not _series_active
        row.operator(MPBI_OT_series.bl_idname, icon='RENDER_STILL')
        if _series_active:
            layout.label(text='Serie in corso: ESC fra i tile', icon='INFO')
    else:
        row = layout.row()
        row.enabled = not _series_active
        row.operator(MPBI_OT_bake.bl_idname, icon='RENDER_STILL')
    layout.separator()
    layout.template_ID(cfg, 'image')
    row = layout.row(align=True)
    row.enabled = cfg.image is not None
    row.operator(MPBI_OT_show.bl_idname, icon='IMAGE_DATA')
    row.operator(MPBI_OT_save.bl_idname, icon='FILE_TICK')


def has_multires(context):
    ob = active_mesh(context)
    return bool(ob and any(m.type == 'MULTIRES' for m in ob.modifiers))


def draw_modifier_bake(layout, context):
    if not has_multires(context) or not native_available():
        return
    box = layout.box()
    box.label(text='Multires Polypaint → Texture UV', icon='IMAGE_DATA')
    draw_bake_controls(box, context)


class MPBI_PT_sidebar(bpy.types.Panel):
    bl_label = 'Polypaint → Texture UV'
    bl_idname = 'MPBI_PT_polypaint_bake_sidebar'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'

    @classmethod
    def poll(cls, context):
        return has_multires(context) and native_available()

    def draw(self, context):
        draw_bake_controls(self.layout, context)


class MPBI_PT_render(bpy.types.Panel):
    bl_label = 'Bake · Multires Polypaint'
    bl_idname = 'MPBI_PT_polypaint_bake_render'
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'render'
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return has_multires(context) and native_available()

    def draw(self, context):
        draw_bake_controls(self.layout, context)


classes = (MPBI_Settings, MPBI_OT_bake, MPBI_OT_cancel_series, MPBI_OT_series, MPBI_OT_show, MPBI_OT_save, MPBI_PT_sidebar, MPBI_PT_render)


def register_props():
    bpy.types.Scene.multires_polypaint_bake_internal = PointerProperty(type=MPBI_Settings)


def unregister_props():
    if hasattr(bpy.types.Scene, 'multires_polypaint_bake_internal'):
        del bpy.types.Scene.multires_polypaint_bake_internal
