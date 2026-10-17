"""Experimental detail projection UI; native C++ writes the resulting layer."""
bl_info = {'name': 'Sculpt Detail Transfer V01', 'author': 'Sculpt Extended',
           'version': (0, 1, 0), 'blender': (5, 2, 0), 'category': 'Sculpt'}

import bpy
import math
import json
import hashlib
import time
from array import array
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from bpy.props import PointerProperty, EnumProperty, IntProperty, FloatProperty, BoolProperty, StringProperty
from . import core

DELTA = 'detail_transfer_delta'
ORIGIN = 'detail_transfer_origin'
TAG = 'detail_transfer_v01'


def mesh_poll(self, ob):
    return ob.type == 'MESH'


def multires(ob):
    mods = [m for m in ob.modifiers if m.type == 'MULTIRES']
    if len(mods) != 1 or len(ob.modifiers) != 1:
        raise ValueError('V01 richiede un solo modificatore Multires, senza altri modificatori')
    return mods[0]


def check_object(ob, limit, *, require_multires=False):
    if ob is None or ob.type != 'MESH' or ob.mode != 'OBJECT':
        raise ValueError('Scegli mesh in Object Mode; termina prima le pennellate Sculpt')
    if ob.library or ob.override_library or ob.data.library or ob.data.shape_keys:
        raise ValueError('V01 richiede mesh locali senza override o Shape Keys')
    if ob.matrix_world.to_3x3().determinant() <= 1e-12:
        raise ValueError('V01 richiede trasformazioni non degeneri, senza scala negativa')
    if any(not math.isfinite(v) for row in ob.matrix_world for v in row):
        raise ValueError('Trasformazione non finita')
    if require_multires:
        m = multires(ob)
        if m.total_levels < 1 or m.total_levels > 13 or not m.show_viewport:
            raise ValueError('Multires deve essere visibile e avere almeno una suddivisione')
        if getattr(m, 'use_sculpt_displacement', False) or getattr(m, 'use_sculpt_shrinkwrap', False):
            raise ValueError('Disattiva displacement e shrinkwrap live per questa prova')
        side = (1 << (m.total_levels-1)) + 1
        estimate = len(ob.data.loops)*side*side
    else:
        if ob.modifiers:
            raise ValueError('Per la coppia esterna usa due mesh senza modificatori')
        estimate = len(ob.data.vertices)
    if estimate > limit:
        raise ValueError(f'Prova V01 limitata a {limit:,} campioni; stima per {ob.name}: {estimate:,}')


def topology(mesh):
    indices = array('i', [0]) * len(mesh.loops)
    sizes = array('i', [0]) * len(mesh.polygons)
    mesh.loops.foreach_get('vertex_index', indices)
    mesh.polygons.foreach_get('loop_total', sizes)
    return (len(mesh.vertices), sizes.tobytes(), indices.tobytes())


def mesh_snapshot(mesh, matrix, limit, *, triangles=False):
    if len(mesh.vertices) > limit:
        raise ValueError('La mesh valutata supera il limite della prova')
    normal_matrix = matrix.to_3x3().inverted().transposed()
    positions, normals = [], []
    for vert in mesh.vertices:
        p = matrix @ vert.co
        n = normal_matrix @ vert.normal
        if not core.finite(p) or not core.finite(n):
            raise ValueError('Coordinate o normali non finite')
        positions.append(tuple(p))
        normals.append(tuple(n.normalized()))
    tri = None
    if triangles:
        mesh.calc_loop_triangles()
        tri = [tuple(t.vertices) for t in mesh.loop_triangles]
        if not tri:
            raise ValueError('La superficie di riferimento non ha triangoli')
    return {'positions': positions, 'normals': normals, 'topology': topology(mesh), 'triangles': tri}


def evaluated_snapshot(ob, limit, *, triangles=False):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = ob.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        return mesh_snapshot(mesh, ob.matrix_world, limit, triangles=triangles)
    finally:
        evaluated.to_mesh_clear()


def source_pair(settings):
    source, limit = settings.source, settings.sample_limit
    if settings.source_mode == 'PAIR':
        reference = settings.reference
        check_object(source, limit)
        check_object(reference, limit)
        if source == reference:
            raise ValueError('Sorgente dettagliata e riferimento devono essere distinti')
        high = evaluated_snapshot(source, limit)
        low = evaluated_snapshot(reference, limit, triangles=True)
    else:
        check_object(source, limit, require_multires=True)
        original_mmd = multires(source)
        if not hasattr(original_mmd, 'sculpt_layers') or not original_mmd.sculpt_layers:
            raise ValueError('La sorgente non ha Sculpt Layers')
        index = settings.source_layer
        if index < 0 or index >= len(original_mmd.sculpt_layers):
            raise ValueError('Indice del layer sorgente non valido (parte da zero)')
        layer = original_mmd.sculpt_layers[index]
        if not layer.visible or not math.isfinite(layer.strength) or abs(layer.strength) < 1e-8:
            raise ValueError('Il layer sorgente deve essere visibile e avere Strength non zero')
        # Duplicate only for evaluation. Original Strength/REC/mesh are never edited.
        clone = source.copy()
        clone_mesh = None
        try:
            clone_mesh = source.data.copy()
            clone.data = clone_mesh
            clone.name = '__DT_SOURCE_TEMP__'
            clone.hide_viewport = False
            clone.hide_render = True
            bpy.context.scene.collection.objects.link(clone)
            clone.hide_set(False)
            clone.select_set(False)
            mmd = multires(clone)
            mmd.levels = mmd.total_levels
            mmd.sculpt_layer_recording = False
            bpy.context.view_layer.update()
            high = evaluated_snapshot(clone, limit)
            mmd.sculpt_layers[index].strength = 0.0
            bpy.context.view_layer.update()
            low = evaluated_snapshot(clone, limit, triangles=True)
        finally:
            bpy.data.objects.remove(clone, do_unlink=True)
            if clone_mesh is not None and clone_mesh.users == 0:
                bpy.data.meshes.remove(clone_mesh)
    if high['topology'] != low['topology']:
        raise ValueError('Sorgente e riferimento devono avere stessa topologia e stesso ordine dei vertici')
    return high, low


def settings_signature(s):
    data = {'source': s.source.name if s.source else None, 'reference': s.reference.name if s.reference else None,
            'target': s.target.name if s.target else None, 'source_mode': s.source_mode,
            'source_layer': s.source_layer, 'mode': s.mode, 'distance': s.max_distance,
            'angle': s.max_angle, 'intensity': s.intensity, 'fade': s.distance_fade}
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()


def build_preview(settings, context):
    target = settings.target
    limit = settings.sample_limit
    check_object(target, limit, require_multires=True)
    if target == settings.source or (settings.source_mode == 'PAIR' and target == settings.reference):
        raise ValueError('La destinazione deve essere un altro oggetto')
    if target.data.users != 1:
        raise ValueError('La destinazione deve avere mesh single-user')
    mmd = multires(target)
    if mmd.levels != mmd.total_levels:
        raise ValueError('Imposta Viewport Levels della destinazione al massimo prima della prova')
    high, low = source_pair(settings)
    ref_positions = low['positions']
    source_deltas = [core.sub(a,b) for a,b in zip(high['positions'], ref_positions)]
    del high
    tree = BVHTree.FromPolygons(ref_positions, low['triangles'], all_triangles=True)
    depsgraph = context.evaluated_depsgraph_get()
    evaluated = target.evaluated_get(depsgraph)
    preview_mesh = None
    preview = None
    try:
        preview_mesh = bpy.data.meshes.new_from_object(evaluated, preserve_all_data_layers=True, depsgraph=depsgraph)
        if len(preview_mesh.vertices) > limit:
            raise ValueError('La destinazione valutata supera il limite della prova')
        n = len(preview_mesh.vertices)
        if not n:
            raise ValueError('Destinazione valutata vuota')
        # Snapshot local coordinates before any preview write; source lookup is in world space.
        origin = array('f', [0.0])*(n*3)
        preview_mesh.vertices.foreach_get('co', origin)
        delta_values = array('f', [0.0])*(n*3)
        positions = array('f', origin)
        matrix = target.matrix_world.copy()
        inverse = matrix.to_3x3().inverted()
        normal_matrix = inverse.transposed()
        cosine_limit = math.cos(settings.max_angle)
        accepted = rejected = nonzero = 0
        maximum = 0.0
        context.window_manager.progress_begin(0, n)
        try:
            for i, vertex in enumerate(preview_mesh.vertices):
                point = matrix @ vertex.co
                normal = normal_matrix @ vertex.normal
                hit, face_normal, tri_index, distance = tree.find_nearest(point, settings.max_distance)
                if tri_index is None:
                    rejected += 1
                    continue
                a,b,c = low['triangles'][tri_index]
                try:
                    weights = core.barycentric(tuple(hit), ref_positions[a], ref_positions[b], ref_positions[c])
                    ref_normal = core.interpolate(weights, low['normals'][a], low['normals'][b], low['normals'][c])
                    delta = core.interpolate(weights, source_deltas[a], source_deltas[b], source_deltas[c])
                    moved = core.transfer_delta(delta, ref_normal, tuple(normal), distance=distance,
                        max_distance=settings.max_distance, cosine_limit=cosine_limit,
                        intensity=settings.intensity, mode=settings.mode, fade=settings.distance_fade)
                except ValueError:
                    moved = None
                if moved is None:
                    rejected += 1
                    continue
                local_delta = inverse @ Vector(moved)
                if not core.finite(local_delta):
                    raise ValueError('Displacement locale non finito')
                accepted += 1
                length = math.sqrt(core.dot(moved,moved))
                maximum = max(maximum,length)
                nonzero += any(v != 0 for v in local_delta)
                for axis in range(3):
                    delta_values[3*i+axis] = local_delta[axis]
                    positions[3*i+axis] = origin[3*i+axis]+local_delta[axis]
                if i % 1024 == 0:
                    context.window_manager.progress_update(i)
        finally:
            context.window_manager.progress_end()
        if not accepted or not nonzero:
            raise ValueError('Nessun dettaglio trasferibile: controlla layer, distanza e orientamento')
        for name, values in ((ORIGIN, origin), (DELTA, delta_values)):
            old = preview_mesh.attributes.get(name)
            if old is not None:
                preview_mesh.attributes.remove(old)
            attr = preview_mesh.attributes.new(name, 'FLOAT_VECTOR', 'POINT')
            attr.data.foreach_set('vector', values)
        preview_mesh.vertices.foreach_set('co', positions)
        preview_mesh.update()
        preview = bpy.data.objects.new('DetailTransfer_Preview', preview_mesh)
        context.scene.collection.objects.link(preview)
        preview.matrix_world = matrix
        preview[TAG] = True
        preview['dt_target_name'] = target.name
        preview['dt_signature'] = settings_signature(settings)
        preview['dt_stats'] = json.dumps({'vertices':n, 'accepted':accepted, 'rejected':rejected,
                                       'nonzero':nonzero, 'max_world_displacement':maximum})
        preview.display_type = 'WIRE'
        preview.show_in_front = True
        settings.preview = preview
        context.view_layer.update()
        print('[DETAIL_TRANSFER_PREVIEW] '+preview['dt_stats'], flush=True)
        return preview
    except Exception:
        if preview is not None:
            bpy.data.objects.remove(preview, do_unlink=True)
        if preview_mesh is not None and preview_mesh.users == 0:
            bpy.data.meshes.remove(preview_mesh)
        raise


class DTSettings(bpy.types.PropertyGroup):
    source: PointerProperty(name='Sorgente dettagliata', type=bpy.types.Object, poll=mesh_poll)
    reference: PointerProperty(name='Riferimento senza dettaglio', type=bpy.types.Object, poll=mesh_poll)
    target: PointerProperty(name='Destinazione Multires', type=bpy.types.Object, poll=mesh_poll)
    preview: PointerProperty(name='Anteprima', type=bpy.types.Object, poll=mesh_poll)
    source_mode: EnumProperty(name='Dettaglio da', items=[('LAYER','Layer Multires','Differenza fra layer visibile e Strength zero su una copia'),
        ('PAIR','Coppia di mesh','Dettagliata e riferimento con identico ordine dei vertici')])
    source_layer: IntProperty(name='Indice layer sorgente', default=0, min=0)
    mode: EnumProperty(name='Trasferimento', items=[('NORMAL','Rilievo normale','Pori, incisioni e rughe'),
        ('VECTOR','Vettore orientato','Ruota il vettore allineando le normali; sperimentale')])
    max_distance: FloatProperty(name='Distanza massima', default=0.05, min=1e-6, subtype='DISTANCE')
    max_angle: FloatProperty(name='Angolo massimo', default=math.radians(60), min=0, max=math.radians(89), subtype='ANGLE')
    intensity: FloatProperty(name='Intensita', default=1, min=-10, max=10)
    distance_fade: BoolProperty(name='Sfuma con la distanza', default=True)
    sample_limit: IntProperty(name='Limite campioni V01', default=200000, min=1000, max=1000000)
    layer_name: StringProperty(name='Nuovo layer', default='Transferred Detail')


class DT_OT_preview(bpy.types.Operator):
    bl_idname = 'sculpt.detail_transfer_preview_v01'
    bl_label = 'Crea anteprima separata'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        if context.mode != 'OBJECT':
            self.report({'ERROR'}, 'Passa in Object Mode prima della prova')
            return {'CANCELLED'}
        try:
            preview = build_preview(context.scene.detail_transfer_v01, context)
        except Exception as exc:
            self.report({'ERROR'}, str(exc))
            print('[DETAIL_TRANSFER_ERROR] '+str(exc), flush=True)
            return {'CANCELLED'}
        self.report({'INFO'}, 'Anteprima creata: '+preview.name)
        return {'FINISHED'}


class DT_OT_apply(bpy.types.Operator):
    bl_idname = 'sculpt.detail_transfer_apply_v01'
    bl_label = 'Conferma in nuovo layer'
    # Native child operator owns the data mutation and its Memfile Undo step.
    bl_options = {'REGISTER'}

    def execute(self, context):
        s = context.scene.detail_transfer_v01
        if context.mode != 'OBJECT' or not s.target or not s.preview:
            self.report({'ERROR'}, 'Richiesti Object Mode, destinazione e anteprima')
            return {'CANCELLED'}
        p = s.preview
        if not p.get(TAG) or p.get('dt_target_name') != s.target.name or p.get('dt_signature') != settings_signature(s):
            self.report({'ERROR'}, 'Parametri/destinazione cambiati: crea una nuova anteprima')
            return {'CANCELLED'}
        try:
            bpy.ops.sculpt.multires_detail_transfer_import_v01.get_rna_type()
        except Exception:
            self.report({'ERROR'}, 'Manca la patch nativa V01: puoi usare soltanto l anteprima')
            return {'CANCELLED'}
        try:
            with context.temp_override(active_object=s.target, object=s.target):
                result = bpy.ops.sculpt.multires_detail_transfer_import_v01(
                    'EXEC_DEFAULT', True,
                    preview_object=p.name, layer_name=s.layer_name)
        except Exception as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        if 'FINISHED' not in result:
            return {'CANCELLED'}
        # Keep the frozen preview for numerical comparison; no geometry is erased.
        p.hide_set(True)
        self.report({'INFO'}, 'Nuovo Sculpt Layer creato; anteprima conservata e nascosta')
        return {'FINISHED'}


class DT_PT_transfer(bpy.types.Panel):
    bl_label = 'Detail Transfer V01'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Multires'

    def draw(self, context):
        s = context.scene.detail_transfer_v01
        layout = self.layout
        layout.label(text='Prova su copie piccole e gia allineate', icon='INFO')
        layout.prop(s,'source_mode')
        layout.prop(s,'source')
        if s.source_mode == 'PAIR':
            layout.prop(s,'reference')
        else:
            layout.prop(s,'source_layer')
            if s.source:
                mods = [m for m in s.source.modifiers if m.type == 'MULTIRES']
                if len(mods)==1 and hasattr(mods[0],'sculpt_layers') and s.source_layer < len(mods[0].sculpt_layers):
                    layout.label(text=mods[0].sculpt_layers[s.source_layer].name)
        layout.prop(s,'target')
        layout.prop(s,'mode')
        layout.prop(s,'max_distance')
        layout.prop(s,'max_angle')
        layout.prop(s,'intensity')
        layout.prop(s,'distance_fade')
        layout.prop(s,'sample_limit')
        col=layout.column()
        col.enabled=context.mode=='OBJECT'
        col.operator(DT_OT_preview.bl_idname)
        layout.prop(s,'preview')
        layout.prop(s,'layer_name')
        col=layout.column()
        col.enabled=context.mode=='OBJECT' and s.preview is not None
        col.operator(DT_OT_apply.bl_idname)
        if s.preview and s.preview.get(TAG):
            stats=json.loads(s.preview.get('dt_stats','{}'))
            layout.label(text=f"Campioni accettati: {stats.get('accepted',0)}")
            layout.label(text=f"Scartati: {stats.get('rejected',0)}")


CLASSES=(DTSettings, DT_OT_preview, DT_OT_apply, DT_PT_transfer)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.detail_transfer_v01 = PointerProperty(type=DTSettings)
    print('[DETAIL_TRANSFER_V01] N > Multires > Detail Transfer V01', flush=True)


def unregister():
    if hasattr(bpy.types.Scene,'detail_transfer_v01'):
        del bpy.types.Scene.detail_transfer_v01
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
