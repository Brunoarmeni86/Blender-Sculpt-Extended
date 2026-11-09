"""Isolated Sculpt/Edit Mode reference prototype. Loaded only by its preview launcher.
Loaded explicitly by the preview launcher; no installation or startup files modified.
"""
import bpy
import bmesh
import gpu
import math
import os
from mathutils import Matrix, Vector, Quaternion, Euler
from gpu_extras.batch import batch_for_shader
from bpy_extras import view3d_utils
from bpy.props import BoolProperty, FloatProperty, FloatVectorProperty, IntProperty, StringProperty, EnumProperty, PointerProperty

TOOL = 'sculpt_extended.symmetry_reference_preview'
EDIT_TOOL = 'sculpt_extended.edit_symmetry_reference_preview'
NATIVE_BACKEND = os.environ.get('BLENDER_SCULPT_SYMMETRY_REFERENCE_V01') == '1'
_session = None
_handle = None
_panels = []
_last_anchor = {}
_anchor_eval_cache = {}
_debug_last_group = None
_modal_reference_preview = None
_native_migration_pending = set()
_colors = ((0.95, .18, .14), (.22, .8, .25), (.22, .45, 1.))
_deform_modifiers = {'ARMATURE', 'LATTICE', 'SIMPLE_DEFORM', 'DISPLACE', 'CAST', 'HOOK', 'SMOOTH', 'CORRECTIVE_SMOOTH'}

def redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D': area.tag_redraw()

def sculpt_object(context):
    ob = context.object
    return ob if ob and ob.type == 'MESH' and context.mode in {'SCULPT','EDIT_MESH'} else None

def editing(context):
    ob = sculpt_object(context)
    if not(ob and _session and _session['pointer'] == ob.as_pointer()):return False
    active=context.workspace.tools.from_space_view3d_mode(context.mode,create=False)
    return bool(active and active.idname in {TOOL,EDIT_TOOL})

def tool_selected(context):
    if not sculpt_object(context):return False
    active=context.workspace.tools.from_space_view3d_mode(context.mode,create=False)
    return bool(active and active.idname in {TOOL,EDIT_TOOL})

_activation_pending=False
def defer_tool_activation(context):
    global _activation_pending
    if _activation_pending or not tool_selected(context) or editing(context):return
    window=context.window;area=context.area;ob=context.object
    region=next((r for r in area.regions if r.type=='WINDOW'),None) if area else None
    if not window or not region:return
    pointer=ob.as_pointer();mode=context.mode
    _activation_pending=True
    def initialize():
        global _activation_pending
        try:
            with bpy.context.temp_override(window=window,area=area,region=region):
                c=bpy.context
                if c.object and c.object.as_pointer()==pointer and c.mode==mode and tool_selected(c):
                    ensure_tool_session(c)
        except (ReferenceError,RuntimeError):pass
        finally:_activation_pending=False
        return None
    # One UI initialization after drawing, not a recurring task/monitor.
    bpy.app.timers.register(initialize,first_interval=0.0)

def ensure_tool_session(context):
    """Called from setup/refresh/invoke, never mutate IDs from a draw/poll callback."""
    global _session
    ob=sculpt_object(context)
    if not ob or not tool_selected(context):return False
    if _session and _session['pointer']==ob.as_pointer():return True
    s=ob.sculpt_symref_preview
    session={'pointer':ob.as_pointer(),'before':snapshot(s),'native_before':native_reference_snapshot(ob),'tool':'builtin.select_box' if context.mode=='EDIT_MESH' else 'builtin.brush','pick':False}
    if not ob.sculpt_symmetry_reference_enabled:ob.sculpt_symmetry_reference_display_size=max(.1,max(ob.dimensions)*.75)
    ob.sculpt_symmetry_reference_enabled=True
    if NATIVE_BACKEND and context.mode=='SCULPT' and ob.sculpt_symmetry_combine_local:ob.sculpt_symmetry_reference_use_for_strokes=True
    _session=session
    redraw();return True


def anchor_supported(ob):
    if ob.mode=='SCULPT' and ob.use_dynamic_topology_sculpting:return False
    for m in ob.modifiers:
        if not m.show_viewport: continue
        if m.type == 'MULTIRES' and m.total_levels == 0: continue
        if m.type not in _deform_modifiers: return False
    return not ob.data.is_editmode

def anchor_position(ob, depsgraph):
    s = ob.sculpt_symref_preview
    if not s.anchored: return None
    mesh = ob.data
    if s.anchor_mesh != mesh:return None
    if not anchor_supported(ob): return None
    if (len(mesh.vertices), len(mesh.edges), len(mesh.polygons)) != tuple(s.topology): return None
    if not 0 <= s.vertex < len(mesh.vertices) or not 0 <= s.face < len(mesh.polygons): return None
    if ','.join(str(v) for v in mesh.polygons[s.face].vertices) != s.face_signature: return None
    key=(ob.as_pointer(),depsgraph.as_pointer(),s.vertex,s.face,tuple(s.topology))
    if key in _anchor_eval_cache:return _anchor_eval_cache[key].copy()
    evaluated = ob.evaluated_get(depsgraph)
    if evaluated.type != 'MESH' or len(evaluated.data.vertices) != len(mesh.vertices): return None
    point=Vector(evaluated.data.vertices[s.vertex].co)
    _anchor_eval_cache[key]=point.copy()
    return point

def invalidate_anchor_cache(scene, depsgraph):
    if any(isinstance(update.id,(bpy.types.Object,bpy.types.Mesh)) for update in depsgraph.updates):
        _anchor_eval_cache.clear()

def ensure_native_reference(ob):
    """Migrate the legacy Python settings once, without tagging object geometry."""
    if ob.sculpt_symmetry_reference_valid and ob.sculpt_symmetry_routing_valid:return True
    def migrate(target):
        s = target.sculpt_symref_preview
        if not target.sculpt_symmetry_reference_valid:
            target.sculpt_symmetry_reference_location = s.location
            target.sculpt_symmetry_reference_rotation = s.rotation
            target.sculpt_symmetry_reference_display_size = max(.01, s.display_size)
            target.sculpt_symmetry_reference_mode = s.mode
            target.sculpt_symmetry_reference_enabled = s.enabled
            target.sculpt_symmetry_reference_use_for_strokes = s.use_for_strokes
            target.sculpt_transform_pivot_location = s.transform_pivot_location
            target.sculpt_transform_pivot_enabled = s.transform_pivot_enabled
            target.sculpt_transform_pivot_linked = s.transform_pivot_linked
            target.sculpt_symmetry_reference_locked = s.locked
            target.sculpt_symmetry_anchor_offset = s.anchor_offset
            target.sculpt_symmetry_reference_valid = True
        if not target.sculpt_symmetry_routing_valid:
            target.sculpt_symmetry_combine_local = s.combine_local
            target.sculpt_symmetry_reference_axes = s.reference_axes
            target.sculpt_symmetry_routing_valid = True
    try:
        migrate(ob);return True
    except AttributeError as ex:
        if 'Writing to ID classes in this context is not allowed' not in str(ex):raise
        pointer=ob.as_pointer()
        if pointer not in _native_migration_pending:
            _native_migration_pending.add(pointer)
            def deferred():
                try:
                    target=next((item for item in bpy.data.objects if item.as_pointer()==pointer),None)
                    if target and not (target.sculpt_symmetry_reference_valid and target.sculpt_symmetry_routing_valid):migrate(target)
                finally:_native_migration_pending.discard(pointer)
                redraw();return None
            bpy.app.timers.register(deferred,first_interval=0.0)
        return False

def native_reference_snapshot(ob):
    ensure_native_reference(ob)
    return (tuple(ob.sculpt_symmetry_reference_location),
            tuple(ob.sculpt_symmetry_reference_rotation),
            ob.sculpt_symmetry_reference_display_size,
            ob.sculpt_symmetry_reference_mode,
            ob.sculpt_symmetry_reference_enabled,
            ob.sculpt_symmetry_reference_use_for_strokes,
            tuple(ob.sculpt_transform_pivot_location),
            ob.sculpt_transform_pivot_enabled,
            ob.sculpt_transform_pivot_linked,
            ob.sculpt_symmetry_reference_locked,
            tuple(ob.sculpt_symmetry_anchor_offset))

def restore_native_reference(ob, state):
    ob.sculpt_symmetry_reference_location = state[0]
    ob.sculpt_symmetry_reference_rotation = state[1]
    ob.sculpt_symmetry_reference_display_size = state[2]
    ob.sculpt_symmetry_reference_mode = state[3]
    ob.sculpt_symmetry_reference_enabled = state[4]
    ob.sculpt_symmetry_reference_use_for_strokes = state[5]
    ob.sculpt_transform_pivot_location = state[6]
    ob.sculpt_transform_pivot_enabled = state[7]
    ob.sculpt_transform_pivot_linked = state[8]
    ob.sculpt_symmetry_reference_locked = state[9]
    ob.sculpt_symmetry_anchor_offset = state[10]

def set_reference_rotation(ob, rotation):
    ensure_native_reference(ob)
    ob.sculpt_symmetry_reference_rotation = rotation
    redraw()

def reference_matrix(ob, depsgraph=None):
    if (_modal_reference_preview and
            _modal_reference_preview['pointer'] == ob.as_pointer() and
            _modal_reference_preview.get('matrix') is not None):
        return _modal_reference_preview['matrix'].copy(), True
    native=ensure_native_reference(ob)
    s = ob.sculpt_symref_preview
    local = Vector(ob.sculpt_symmetry_reference_location if native else s.location)
    valid = True
    if s.anchored:
        point = anchor_position(ob, depsgraph or bpy.context.evaluated_depsgraph_get())
        valid = point is not None
        key=(ob.as_pointer(),s.vertex,s.face)
        if valid:
            _last_anchor[key]=point.copy()
            local = point + Vector(ob.sculpt_symmetry_anchor_offset if native else s.anchor_offset)
        elif key in _last_anchor:local=_last_anchor[key]+Vector(ob.sculpt_symmetry_anchor_offset if native else s.anchor_offset)
    rotation = ob.matrix_world.to_quaternion() @ Euler(ob.sculpt_symmetry_reference_rotation if native else s.rotation, 'XYZ').to_quaternion()
    result = rotation.to_matrix().to_4x4()
    result.translation = ob.matrix_world @ local
    return result, valid

def reference_display_size(ob):
    if (_modal_reference_preview and
            _modal_reference_preview['pointer'] == ob.as_pointer() and
            _modal_reference_preview.get('size') is not None):
        return _modal_reference_preview['size']
    return ob.sculpt_symmetry_reference_display_size if ensure_native_reference(ob) else ob.sculpt_symref_preview.display_size

def set_modal_reference_preview(ob, matrix=None, size=None):
    """Viewport-only G/R/S state. Never writes an Object RNA property."""
    global _modal_reference_preview
    current = (_modal_reference_preview if _modal_reference_preview and
               _modal_reference_preview['pointer'] == ob.as_pointer() else
               {'pointer': ob.as_pointer(), 'matrix': None, 'size': None})
    changed=False
    if matrix is not None:
        old=current['matrix']
        changed=old is None or any(abs(old[i][j]-matrix[i][j])>1e-8 for i in range(4) for j in range(4))
        if changed:current['matrix']=matrix.copy()
    if size is not None:
        value=float(size)
        if current['size'] is None or abs(current['size']-value)>1e-8:
            current['size']=value;changed=True
    if changed or _modal_reference_preview is None:
        _modal_reference_preview = current
        redraw()

def clear_modal_reference_preview(ob=None):
    global _modal_reference_preview
    if ob is None or (_modal_reference_preview and
                      _modal_reference_preview['pointer'] == ob.as_pointer()):
        _modal_reference_preview = None
    redraw()

def set_world_location(ob, point):
    ensure_native_reference(ob)
    s = ob.sculpt_symref_preview
    if ob.sculpt_symmetry_reference_locked:return False
    local = ob.matrix_world.inverted_safe() @ Vector(point)
    if s.anchored:
        anchor = anchor_position(ob, bpy.context.evaluated_depsgraph_get())
        if anchor is None: return False
        ob.sculpt_symmetry_anchor_offset = local - anchor
    ob.sculpt_symmetry_reference_location = local
    redraw()
    return True

def from_cursor(context):
    ob = sculpt_object(context); s = ob.sculpt_symref_preview
    matrix = context.scene.cursor.matrix.copy()
    set_world_location(ob, matrix.translation)
    set_reference_rotation(ob, (ob.matrix_world.to_quaternion().inverted() @ matrix.to_quaternion()).to_euler('XYZ'))

def snapshot(s):
    return {p.identifier: (tuple(getattr(s,p.identifier)) if getattr(p,'is_array',False) else getattr(s,p.identifier))
            for p in s.bl_rna.properties if p.identifier not in {'rna_type', 'name'} and not p.is_readonly}

_combine_update_guard = False


def update_combine_local(settings, context):
    """Leaving double symmetry returns to native local mirroring by default.

    The separate 'Use reference for brushes' control remains available for an
    explicit reference-only mode after disabling Combine. This prevents an
    implicit reinterpretation of object-local X/Y/Z axes in the reference frame.
    Stroke passes are frozen at stroke start by the native Sculpt path.
    """
    global _combine_update_guard
    if _combine_update_guard:
        redraw()
        return

    ob=sculpt_object(context)
    # A normal RNA checkbox writes the new value before entering its update
    # callback, but dependency-graph evaluation is still deferred. Commit the
    # live CCG through the Sculpt-Layer-aware path in this window, before the
    # Object metadata change can be evaluated by object_update_from_subsurf_ccg.
    # The commit is a no-op fast path when no stroke coordinates are pending.
    if ob is not None and context.mode=='SCULPT':
        multires=next((m for m in ob.modifiers if m.type=='MULTIRES'),None)
        if multires and len(multires.sculpt_layers):
            result=bpy.ops.sculpt.multires_layer_commit()
            if result!={'FINISHED'}:
                # RNA update callbacks cannot report/cancel. Restore the old
                # Boolean without re-entering this transaction instead.
                _combine_update_guard=True
                try:
                    settings.combine_local=not settings.combine_local
                finally:
                    _combine_update_guard=False
                redraw()
                return
    if NATIVE_BACKEND and ob is not None:
        local_active=any(getattr(ob.data,'use_mirror_'+axis,False) for axis in 'xyz')
        if settings.combine_local:
            # Keep the legacy and native channels coherent while an older file is
            # being migrated. C++ may read either channel until `valid` is set.
            settings.use_for_strokes=True
            ensure_native_reference(ob)
            ob.sculpt_symmetry_reference_use_for_strokes=True
        elif local_active:
            settings.use_for_strokes=False
            ensure_native_reference(ob)
            ob.sculpt_symmetry_reference_use_for_strokes=False
    redraw()


class SculptSymmetryReferencePreview(bpy.types.PropertyGroup):
    edit_direction: EnumProperty(name='Source Side',items=[(v,label,'Copy this side to the opposite side') for v,label in [('-X','−X → +X'),('X','+X → −X'),('-Y','−Y → +Y'),('Y','+Y → −Y'),('-Z','−Z → +Z'),('Z','+Z → −Z')]],default='-X',update=lambda s,c:redraw())
    edit_threshold: FloatProperty(name='Merge Threshold',default=.0001,min=0,max=1,subtype='DISTANCE')
    edit_selected_only: BoolProperty(name='Selected Only',default=True)
    locked: BoolProperty(name='Lock Reference',description='Protect settings; an active attachment continues to follow its vertex',default=False,update=lambda s,c:redraw())
    use_for_strokes: BoolProperty(name='Use Reference for Brushes', default=False, update=lambda s,c:redraw())
    transform_pivot_enabled: BoolProperty(name='Show Transform Pivot', description='Show a movable pivot for the Move, Rotate and Scale brushes', default=False, update=lambda s,c:redraw())
    transform_pivot_linked: BoolProperty(name='Share with Symmetry', description='Also use the symmetry reference as the transform brush pivot', default=True, update=lambda s,c:redraw())
    transform_pivot_location: FloatVectorProperty(name='Transform Pivot', subtype='TRANSLATION', update=lambda s,c:redraw())
    combine_local: BoolProperty(name='Combine Local Symmetry', default=False, update=update_combine_local)
    reference_axes: bpy.props.BoolVectorProperty(name='Reference Axes', size=3, default=(True,False,False), subtype='XYZ', update=lambda s,c:redraw())
    enabled: BoolProperty(name='Show Planes', default=False, update=lambda s,c:redraw())
    location: FloatVectorProperty(name='Position', subtype='TRANSLATION', update=lambda s,c:redraw())
    rotation: FloatVectorProperty(name='Rotation', subtype='EULER', update=lambda s,c:redraw())
    display_size: FloatProperty(name='Plane Size', default=2., min=.01, max=100000., subtype='DISTANCE', update=lambda s,c:redraw())
    opacity: FloatProperty(name='Opacity', default=.12, min=.01, max=.5, update=lambda s,c:redraw())
    mode: EnumProperty(name='Gizmo', items=[('MOVE','Move',''),('ROTATE','Rotate',''),('SIZE','Size','Plane display only')], default='MOVE', update=lambda s,c:redraw())
    anchored: BoolProperty(default=False)
    vertex: IntProperty(default=-1)
    face: IntProperty(default=-1)
    topology: bpy.props.IntVectorProperty(size=3, default=(0,0,0))
    face_signature: StringProperty()
    anchor_offset: FloatVectorProperty()
    anchor_mesh: PointerProperty(type=bpy.types.Mesh)

class SCULPT_OT_symref_edit_preview(bpy.types.Operator):
    bl_idname='sculpt.symref_edit_preview'
    bl_label='Edit Reference'
    bl_description='Set the position and rotation of the Sculpt symmetry reference'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,c): return bool(sculpt_object(c)) and not editing(c) and not c.object.sculpt_symmetry_reference_locked
    def execute(self,c):
        global _session
        ob=sculpt_object(c);s=ob.sculpt_symref_preview
        active=c.workspace.tools.from_space_view3d_mode(c.mode,create=False)
        _session={'pointer':ob.as_pointer(),'before':snapshot(s),'native_before':native_reference_snapshot(ob),'tool':active.idname if active else 'builtin.brush','pick':False}
        if not ob.sculpt_symmetry_reference_enabled:
            ob.sculpt_symmetry_reference_display_size=max(.1,max(ob.dimensions)*.75)
        ob.sculpt_symmetry_reference_enabled=True
        if NATIVE_BACKEND and c.mode=='SCULPT' and ob.sculpt_symmetry_combine_local:ob.sculpt_symmetry_reference_use_for_strokes=True
        try: bpy.ops.wm.tool_set_by_id(name=EDIT_TOOL if c.mode=='EDIT_MESH' else TOOL)
        except Exception:
            _session=None
            raise
        redraw();return {'FINISHED'}

class SCULPT_OT_symref_finish_preview(bpy.types.Operator):
    bl_idname='sculpt.symref_finish_preview';bl_label='Confirm Reference'
    bl_options={'REGISTER','UNDO'}
    cancel: BoolProperty(default=False)
    @classmethod
    def poll(cls,c): return editing(c)
    def execute(self,c):
        global _session
        data=_session;ob=sculpt_object(c)
        if self.cancel:
            for key,value in data['before'].items(): setattr(ob.sculpt_symref_preview,key,value)
            restore_native_reference(ob,data['native_before'])
        # Confirming a gizmo edit must not silently re-enable custom symmetry.
        # The explicit Use Reference / Combine toggles own stroke-symmetry state.
        _session=None
        try: bpy.ops.wm.tool_set_by_id(name=data['tool'])
        except RuntimeError: bpy.ops.wm.tool_set_by_id(name='builtin.select_box' if c.mode=='EDIT_MESH' else 'builtin.brush')
        redraw();return {'FINISHED'}

class SCULPT_OT_symref_cursor_preview(bpy.types.Operator):
    bl_idname='sculpt.symref_cursor_preview';bl_label='Dal cursore 3D'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,c): return bool(sculpt_object(c)) and not c.object.sculpt_symmetry_reference_locked
    def execute(self,c):
        s=c.object.sculpt_symref_preview
        if s.anchored and anchor_position(c.object,c.evaluated_depsgraph_get()) is None:
            self.report({'WARNING'},'Invalid attachment: detach the reference before moving it');return {'CANCELLED'}
        from_cursor(c);c.object.sculpt_symmetry_reference_enabled=True
        # Moving the reference from the cursor does not change the brush symmetry mode.
        return {'FINISHED'}

class SCULPT_OT_symref_anchor_preview(bpy.types.Operator):
    bl_idname='sculpt.symref_anchor_preview';bl_label='Choose Anchor Vertex'
    @classmethod
    def poll(cls,c): return editing(c) and anchor_supported(c.object) and not c.object.sculpt_symmetry_reference_locked
    def execute(self,c):
        _session['pick']=True
        self.report({'INFO'},'Click the mesh to choose a vertex near the surface');return {'FINISHED'}

class SCULPT_OT_symref_detach_preview(bpy.types.Operator):
    bl_idname='sculpt.symref_detach_preview';bl_label='Sgancia'
    bl_options={'REGISTER','UNDO'}
    @classmethod
    def poll(cls,c): return bool(sculpt_object(c)) and c.object.sculpt_symref_preview.anchored and not c.object.sculpt_symmetry_reference_locked
    def execute(self,c):
        ob=c.object;matrix,_=reference_matrix(ob,c.evaluated_depsgraph_get())
        ob.sculpt_symref_preview.anchored=False
        set_world_location(ob,matrix.translation);return {'FINISHED'}

def bind_anchor(ob,vertex,face,context):
    s=ob.sculpt_symref_preview;matrix,_=reference_matrix(ob,context.evaluated_depsgraph_get())
    s.vertex=vertex;s.face=face
    s.anchor_mesh=ob.data
    s.topology=(len(ob.data.vertices),len(ob.data.edges),len(ob.data.polygons))
    s.face_signature=','.join(str(v) for v in ob.data.polygons[face].vertices)
    s.anchored=True
    point=anchor_position(ob,context.evaluated_depsgraph_get())
    if point is None:
        s.anchored=False;raise RuntimeError('Invalid vertex correspondence')
    ob.sculpt_symmetry_anchor_offset=ob.matrix_world.inverted_safe()@matrix.translation-point
    ensure_native_reference(ob)
    ob.sculpt_symmetry_reference_location=ob.matrix_world.inverted_safe()@matrix.translation

class SCULPT_OT_symref_place_preview(bpy.types.Operator):
    bl_idname='sculpt.symref_place_preview';bl_label='Position Reference'
    @classmethod
    def poll(cls,c): return editing(c) and not c.object.sculpt_symmetry_reference_locked
    def invoke(self,c,event):
        ob=c.object
        if _session['pick']:
            if not anchor_supported(ob):return {'CANCELLED'}
            mouse=(event.mouse_region_x,event.mouse_region_y)
            origin=view3d_utils.region_2d_to_origin_3d(c.region,c.region_data,mouse)
            direction=view3d_utils.region_2d_to_vector_3d(c.region,c.region_data,mouse)
            inverse=ob.matrix_world.inverted_safe()
            ev=ob.evaluated_get(c.evaluated_depsgraph_get())
            hit,position,normal,index=ev.ray_cast(inverse@origin,(inverse.to_3x3()@direction).normalized())
            if not hit or not 0<=index<len(ob.data.polygons):return {'CANCELLED'}
            if len(ev.data.vertices)!=len(ob.data.vertices):return {'CANCELLED'}
            vertex=min(ev.data.polygons[index].vertices,key=lambda i:(ev.data.vertices[i].co-position).length_squared)
            bind_anchor(ob,vertex,index,c);_session['pick']=False;redraw();return {'FINISHED'}
        old_cursor=c.scene.cursor.matrix.copy()
        try:
            result=bpy.ops.view3d.cursor3d('INVOKE_DEFAULT')
            if result=={'FINISHED'}:set_world_location(ob,c.scene.cursor.location)
        finally:c.scene.cursor.matrix=old_cursor
        return {'FINISHED'}

class VIEW3D_GGT_sculpt_symref_preview(bpy.types.GizmoGroup):
    bl_idname='VIEW3D_GGT_sculpt_symref_preview';bl_label='Symmetry Reference'
    bl_space_type='VIEW_3D';bl_region_type='WINDOW';bl_options={'3D','PERSISTENT','TOOL_FALLBACK_KEYMAP'}
    @classmethod
    def poll(cls,c):return tool_selected(c) and not c.object.sculpt_symmetry_reference_locked
    @classmethod
    def setup_keymap(cls,keyconfig):
        km=keyconfig.keymaps.new(name='Sculpt Symmetry Reference Gizmo',space_type='EMPTY',region_type='WINDOW')
        if not any(k.idname=='gizmogroup.gizmo_tweak' for k in km.keymap_items):
            km.keymap_items.new('gizmogroup.gizmo_tweak','LEFTMOUSE','PRESS',any=True)
        return km
    def setup(self,c):
        defer_tool_activation(c)
        global _debug_last_group
        _debug_last_group=self
        self.handles=[];self.frames={}
        for mode in ('MOVE','ROTATE','SIZE'):
            for axis in range(3):
                gz=self.gizmos.new('GIZMO_GT_dial_3d' if mode=='ROTATE' else 'GIZMO_GT_arrow_3d')
                if mode!='ROTATE':gz.draw_style='BOX' if mode=='SIZE' else 'NORMAL'
                gz.color=_colors[axis];gz.alpha=.9;gz.color_highlight=(1.,1.,1.);gz.alpha_highlight=1.
                gz.scale_basis=1.;gz.use_draw_modal=True
                gz.use_event_handle_all=True
                props=gz.target_set_operator('sculpt.reference_native_transform_preview')
                props.mode={'MOVE':'G','ROTATE':'R','SIZE':'S'}[mode]
                props.target='REFERENCE';props.constraint_axis=tuple(i==axis for i in range(3));props.release_confirm=True
                self.handles.append((mode,axis,gz))
    def set_value(self,mode,axis,value):
        c=bpy.context
        if not editing(c) or (mode,axis) not in self.frames:return
        frame,size=self.frames[(mode,axis)];ob=c.object;s=ob.sculpt_symref_preview
        if ob.sculpt_symmetry_reference_locked:return
        direction=frame.to_3x3().col[axis].normalized()
        if mode=='MOVE':set_world_location(ob,frame.translation+direction*value)
        elif mode=='ROTATE':
            rotation=Quaternion(direction,value)@frame.to_quaternion()
            set_reference_rotation(ob,(ob.matrix_world.to_quaternion().inverted()@rotation).to_euler('XYZ'))
        else:
            ensure_native_reference(ob)
            ob.sculpt_symmetry_reference_display_size=max(.01,size+value)
        redraw()
    def refresh(self,c):
        global _debug_last_group
        _debug_last_group=self
        defer_tool_activation(c)
    def draw_prepare(self,c):
        matrix,valid=reference_matrix(c.object,c.evaluated_depsgraph_get());s=c.object.sculpt_symref_preview
        display_size=reference_display_size(c.object)
        for mode,axis,gz in self.handles:
            gz.hide=mode!=c.object.sculpt_symmetry_reference_mode or not valid
            if gz.is_modal:continue
            self.frames[(mode,axis)]=(matrix.copy(),display_size)
            direction=matrix.to_3x3().col[axis].normalized()
            basis=Vector((0,0,1)).rotation_difference(direction).to_matrix().to_4x4()
            basis.translation=matrix.translation;gz.matrix_basis=basis

class SCULPT_OT_symref_reset_preview(bpy.types.Operator):
    bl_idname='sculpt.symref_reset_preview';bl_label='Reset Reference'
    bl_options={'REGISTER','UNDO'}
    what: EnumProperty(items=[('POSITION','Position','Return to the object origin and detach'),('ROTATION','Rotation','Align to the object local axes'),('ALL','All','Reset position and rotation and detach the reference')],default='ALL')
    @classmethod
    def poll(cls,c):return bool(sculpt_object(c)) and not c.object.sculpt_symmetry_reference_locked
    def execute(self,c):
        s=c.object.sculpt_symref_preview
        if self.what in {'POSITION','ALL'}:
            s.anchored=False;s.anchor_mesh=None;s.vertex=-1;s.face=-1
            ensure_native_reference(c.object);c.object.sculpt_symmetry_reference_location=(0,0,0);c.object.sculpt_symmetry_anchor_offset=(0,0,0)
        if self.what in {'ROTATION','ALL'}:set_reference_rotation(c.object,(0,0,0))
        c.object.sculpt_symmetry_reference_enabled=True;redraw();return {'FINISHED'}

def draw_settings(layout,c):
    ob=sculpt_object(c)
    if not ob:return
    s=ob.sculpt_symref_preview
    box=layout.box();box.label(text='Symmetry Reference · Preview')
    if NATIVE_BACKEND and c.mode=='SCULPT':
        ensure_native_reference(ob);box.prop(ob,'sculpt_symmetry_reference_use_for_strokes',text='Use Reference for Brushes')
        box.prop(ob,'sculpt_symmetry_combine_local')
        if not ob.sculpt_symmetry_combine_local and any(getattr(ob.data,'use_mirror_'+axis,False) for axis in 'xyz'):
            box.label(text='Local symmetry only (enable Use Reference to change it)',icon='INFO')
        if ob.sculpt_symmetry_combine_local:
            box.prop(ob,'sculpt_symmetry_reference_axes',toggle=True)
            row=box.row(align=True);row.label(text='Local Axes')
            for axis in 'xyz':row.prop(ob.data,'use_mirror_'+axis,text=axis.upper(),toggle=True)
            box.label(text='One axis per reference · up to 4 brush impressions')
        box.label(text='Brushes · Mesh / Multires / Dyntopo',icon='INFO')
    elif c.mode=='EDIT_MESH':
        box.prop(s,'edit_direction')
        box.prop(s,'edit_threshold')
        box.prop(s,'edit_selected_only')
        box.operator('mesh.symmetrize_reference_preview',text='Symmetrize · Mask',icon='MOD_MIRROR')
        box.operator('mesh.symmetrize_reference_shape_preview')
        box.label(text='Rebuilds topology on the destination side',icon='INFO')
    ensure_native_reference(ob);box.prop(ob,'sculpt_symmetry_reference_enabled',text='Show Planes')
    if c.mode=='SCULPT':
        box.prop(s,'edit_direction',text='Symmetrize: Source')
        if any(m.type=='MULTIRES' for m in ob.modifiers):
            multires=next(m for m in ob.modifiers if m.type=='MULTIRES')
            controls=box.column(align=True)
            controls.enabled=multires.total_levels>0 and multires.sculpt_levels==multires.total_levels and not multires.use_sculpt_base_mesh
            controls.operator('sculpt.symmetrize_reference_multires_detail',text='Symmetrize · Layer Detail').base_surface=False
            controls.operator('sculpt.symmetrize_reference_multires_detail',text='Symmetrize · base Multires').base_surface=True
            if not controls.enabled:box.label(text='Use the highest Sculpt level · disable Sculpt Base Mesh',icon='INFO')
        else:
            box.operator('mesh.symmetrize_reference_preview',text='Symmetrize · Mask',icon='MOD_MIRROR')
            box.operator('mesh.symmetrize_reference_shape_preview')
    box.prop(ob,'sculpt_symmetry_reference_locked',text='Lock Reference',icon='LOCKED' if ob.sculpt_symmetry_reference_locked else 'UNLOCKED',toggle=True)
    if editing(c):
        ensure_native_reference(ob);row=box.row(align=True);row.enabled=not ob.sculpt_symmetry_reference_locked;row.prop(ob,'sculpt_symmetry_reference_mode',expand=True)
        row=box.row(align=True);row.enabled=not ob.sculpt_symmetry_reference_locked
        row.operator('sculpt.reference_native_transform_preview',text='Move · G').mode='G'
        row.operator('sculpt.reference_native_transform_preview',text='Ruota · R').mode='R'
        row.operator('sculpt.reference_native_transform_preview',text='Scala · S').mode='S'
        row=box.row(align=True);row.prop(c.scene.tool_settings,'use_snap',text='Snap',icon='SNAP_ON');row.prop(c.scene.tool_settings,'snap_elements',text='')
        box.label(text='X/Y/Z · type a number · Ctrl: snap · Esc: cancel')
        row=box.row(align=True);row.operator('sculpt.symref_finish_preview',text='Confirm',icon='CHECKMARK');row.operator('sculpt.symref_finish_preview',text='Cancel',icon='X').cancel=True
        if _session['pick']:box.label(text='Click the mesh to choose a vertex',icon='RESTRICT_SELECT_OFF')
    else:box.operator('sculpt.symref_edit_preview',icon='ORIENTATION_GIMBAL')
    box.operator('sculpt.symref_cursor_preview',icon='PIVOT_CURSOR')
    ensure_native_reference(ob)
    if ob.sculpt_symmetry_reference_enabled:
        row=box.column();row.enabled=not s.anchored and not ob.sculpt_symmetry_reference_locked;row.prop(ob,'sculpt_symmetry_reference_location',text='Position')
        if s.anchored:
            row=box.row();row.enabled=not ob.sculpt_symmetry_reference_locked;row.prop(ob,'sculpt_symmetry_anchor_offset',text='Offset ancoraggio')
        row=box.row();row.enabled=not ob.sculpt_symmetry_reference_locked;row.prop(ob,'sculpt_symmetry_reference_rotation',text='Rotation')
        row=box.row(align=True);row.enabled=not ob.sculpt_symmetry_reference_locked
        row.operator('sculpt.symref_reset_preview',text='Reset pos.').what='POSITION'
        row.operator('sculpt.symref_reset_preview',text='Reset rot.').what='ROTATION'
        row.operator('sculpt.symref_reset_preview',text='Reset tutto').what='ALL'
        ensure_native_reference(ob)
        box.prop(ob,'sculpt_symmetry_reference_display_size',text='Plane Size');box.prop(s,'opacity')
        if s.anchored:
            _,valid=reference_matrix(ob,c.evaluated_depsgraph_get())
            box.label(text=('Vertex '+str(s.vertex)) if valid else 'Attachment suspended: geometry changed',icon='LINKED' if valid else 'ERROR')
            box.operator('sculpt.symref_detach_preview',icon='UNLINKED')
        elif editing(c):
            row=box.row();row.enabled=anchor_supported(ob);row.operator('sculpt.symref_anchor_preview',icon='VERTEXSEL')
            if not row.enabled:box.label(text='Ancoraggio iniziale: mesh senza suddivisioni',icon='INFO')

class SculptSymmetryReferenceTool(bpy.types.WorkSpaceTool):
    bl_space_type='VIEW_3D';bl_context_mode='SCULPT';bl_idname=TOOL
    bl_label='Symmetry Reference';bl_description='Set planes and gizmos without modifying the mesh'
    bl_icon='ops.transform.transform';bl_widget='VIEW3D_GGT_sculpt_symref_preview'
    bl_keymap=(('sculpt.reference_native_transform_preview',{'type':'G','value':'PRESS'},{'properties':[('mode','G')]}),
               ('sculpt.reference_native_transform_preview',{'type':'R','value':'PRESS'},{'properties':[('mode','R')]}),
               ('sculpt.reference_native_transform_preview',{'type':'S','value':'PRESS'},{'properties':[('mode','S')]}),
               ('sculpt.symref_place_preview',{'type':'LEFTMOUSE','value':'CLICK'},None),
               ('sculpt.symref_place_preview',{'type':'RIGHTMOUSE','value':'PRESS','shift':True},None),
               ('sculpt.symref_finish_preview',{'type':'RET','value':'PRESS'},None),
               ('sculpt.symref_finish_preview',{'type':'ESC','value':'PRESS'},{'properties':[('cancel',True)]}))
    @staticmethod
    def draw_settings(c,layout,tool):
        defer_tool_activation(c)
        if sculpt_object(c):
            ensure_native_reference(c.object);row=layout.row(align=True);row.enabled=not c.object.sculpt_symmetry_reference_locked;row.prop(c.object,'sculpt_symmetry_reference_mode',expand=True)
        row=layout.row(align=True)
        row.operator('sculpt.symref_cursor_preview',text='',icon='PIVOT_CURSOR')
        row.operator('sculpt.symref_finish_preview',text='Confirm',icon='CHECKMARK')
        row.operator('sculpt.symref_finish_preview',text='',icon='X').cancel=True
        row.popover(panel='VIEW3D_PT_edit_symmetry_reference_preview' if c.mode=='EDIT_MESH' else 'VIEW3D_PT_sculpt_symmetry_for_topbar',text='',icon='PREFERENCES')

def draw_panel(self,c):draw_settings(self.layout,c)

def draw_planes():
    c=bpy.context;ob=sculpt_object(c)
    if not ob or not c.space_data.overlay.show_overlays:return
    defer_tool_activation(c)
    s=ob.sculpt_symref_preview
    ensure_native_reference(ob)
    if not ob.sculpt_symmetry_reference_enabled:return
    matrix,valid=reference_matrix(ob,c.evaluated_depsgraph_get())
    display_size=reference_display_size(ob)
    shader=gpu.shader.from_builtin('UNIFORM_COLOR')
    old_blend=gpu.state.blend_get();old_depth=gpu.state.depth_test_get();old_mask=gpu.state.depth_mask_get()
    gpu.state.blend_set('ALPHA');gpu.state.depth_test_set('LESS_EQUAL');gpu.state.depth_mask_set(False)
    try:
        axes=tuple(a==s.edit_direction[-1] for a in 'XYZ') if c.mode=='EDIT_MESH' else (ob.sculpt_symmetry_reference_axes if ob.sculpt_symmetry_combine_local else (ob.data.use_mirror_x,ob.data.use_mirror_y,ob.data.use_mirror_z))
        for axis,enabled in enumerate(axes):
            if not enabled:continue
            u=matrix.to_3x3().col[(axis+1)%3]*display_size*.5
            v=matrix.to_3x3().col[(axis+2)%3]*display_size*.5
            center=matrix.translation
            points=[center-u-v,center+u-v,center+u+v,center-u+v]
            color=_colors[axis] if valid else (.8,.5,.1)
            shader.bind();shader.uniform_float('color',(*color,s.opacity))
            batch_for_shader(shader,'TRIS',{'pos':points},indices=((0,1,2),(0,2,3))).draw(shader)
            shader.uniform_float('color',(*color,.8))
            batch_for_shader(shader,'LINE_STRIP',{'pos':points+[points[0]]}).draw(shader)
            if c.mode=='EDIT_MESH':
                sign=-1 if s.edit_direction.startswith('-') else 1
                end=center+matrix.to_3x3().col[axis]*display_size*.3*sign
                shader.uniform_float('color',(1.,.8,.15,1.))
                batch_for_shader(shader,'LINES',{'pos':[center,end]}).draw(shader)
    finally:
        gpu.state.depth_mask_set(old_mask);gpu.state.depth_test_set(old_depth);gpu.state.blend_set(old_blend)


class EditSymmetryReferenceTool(SculptSymmetryReferenceTool):
    bl_context_mode='EDIT_MESH'
    bl_idname=EDIT_TOOL

class MESH_OT_symmetrize_reference_preview(bpy.types.Operator):
    bl_idname='mesh.symmetrize_reference_preview'
    bl_label='Symmetrize · Protected Mask'
    bl_description='Rebuild topology from the source side when possible; with a mask, use transfer or protected stitching'
    bl_options={'REGISTER','UNDO'}
    space:EnumProperty(name='Axes',items=(('LOCAL','Local','Object axes and origin'),('REFERENCE','Custom','Symmetry reference planes')),default='LOCAL')
    respect_mask:BoolProperty(name='Protect Mask',default=True,description='Exclude the masked source from copying and preserve the protected destination')
    transition_width:FloatProperty(name='Stitch Width',default=.25,min=.02,max=.5,subtype='FACTOR',description='Relative width of the editable band connecting the mask to the mirrored copy')
    @classmethod
    def poll(cls,c):return c.mode in {'EDIT_MESH','SCULPT'} and c.object and c.object.type=='MESH'
    def invoke(self,c,event):
        # A visible symmetry gizmo is the intended frame by default.
        ob=c.object
        self.space='REFERENCE' if ob.sculpt_symmetry_reference_enabled and ensure_native_reference(ob) else 'LOCAL'
        return c.window_manager.invoke_props_dialog(self)
    def draw(self,c):
        self.layout.prop(self,'space');self.layout.prop(c.object.sculpt_symref_preview,'edit_direction',text='Source');self.layout.prop(self,'respect_mask');self.layout.prop(self,'transition_width');self.layout.label(text='Gizmo Plane' if self.space=='REFERENCE' else 'Object Local Plane',icon='ORIENTATION_GIMBAL')
        self.layout.label(text='Partial mask: copy shape · different topology: try stitching',icon='INFO')
        self.layout.prop(c.object.sculpt_symref_preview,'edit_threshold')
        if c.mode=='EDIT_MESH':self.layout.prop(c.object.sculpt_symref_preview,'edit_selected_only')
    def execute(self,c):
        from pathlib import Path
        import importlib.util
        ob=c.object;s=ob.sculpt_symref_preview;sculpt=c.mode=='SCULPT'
        if ob.data.users!=1 or ob.data.library or ob.data.shape_keys or any(m.type=='MULTIRES' for m in ob.modifiers) or (sculpt and ob.use_dynamic_topology_sculpting):
            self.report({'ERROR'},'Ricostruzione: usa una mesh non condivisa senza Multires, shape key o Dyntopo');return {'CANCELLED'}
        frame,valid=(ob.matrix_world,True) if self.space=='LOCAL' else reference_matrix(ob,c.evaluated_depsgraph_get())
        if not valid or abs(ob.matrix_world.determinant())<1e-12:
            self.report({'ERROR'},'Invalid reference');return {'CANCELLED'}
        if s.anchored:
            self.report({'ERROR'},'Detach the reference before rebuilding topology');return {'CANCELLED'}
        spec=importlib.util.spec_from_file_location('symmetry_topology_mask',Path(__file__).with_name('symmetry_topology_mask.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        working=None
        try:
            if sculpt:bpy.ops.object.mode_set(mode='OBJECT');working=bmesh.new();working.from_mesh(ob.data)
            else:working=bmesh.from_edit_mesh(ob.data).copy()
            try:
                count,method=module.reconstruct(working,frame.inverted()@ob.matrix_world,s.edit_direction,s.edit_threshold,self.respect_mask,s.edit_selected_only and not sculpt)
            except ValueError as ex:
                if not self.respect_mask or 'different topology' not in str(ex):raise
                if not sculpt and s.edit_selected_only and not all(f.select for f in working.faces):raise ValueError('Stitching: select the entire mesh; the mask protects the region to preserve')
                transition_spec=importlib.util.spec_from_file_location('transition_build',Path(__file__).with_name('symmetry_transition_build.py'));transition=importlib.util.module_from_spec(transition_spec);transition_spec.loader.exec_module(transition)
                replacement,details=transition.assemble(working,frame.inverted()@ob.matrix_world,'XYZ'.index(s.edit_direction[-1]),-1 if s.edit_direction.startswith('-') else 1,self.transition_width)
                working.free();working=replacement;count=details['fixed_faces'];method='topology-band'

            if sculpt:working.to_mesh(ob.data);ob.data.update()
            else:
                # Publish only after all checks; leave Edit Mode to replace topology safely.
                bpy.ops.object.mode_set(mode='OBJECT');working.to_mesh(ob.data);ob.data.update();bpy.ops.object.mode_set(mode='EDIT')
            description=('transferred vertices (topology unchanged)' if method=='shape' else
                         'protected faces during stitching' if method=='topology-band' else
                         'protected faces during topology copying')
            self.report({'INFO'},f'Symmetrize · {count} {description}');return {'FINISHED'}
        except (ValueError,RuntimeError) as ex:
            self.report({'ERROR'},str(ex));return {'CANCELLED'}
        finally:
            if working:working.free()
            if sculpt and ob.mode!='SCULPT':bpy.ops.object.mode_set(mode='SCULPT')

class VIEW3D_PT_edit_symmetry_reference_preview(bpy.types.Panel):
    bl_label='Symmetry Reference'
    bl_space_type='VIEW_3D';bl_region_type='UI';bl_category='Tool'
    @classmethod
    def poll(cls,c):return c.mode=='EDIT_MESH' and c.object and c.object.type=='MESH'
    def draw(self,c):draw_settings(self.layout,c)

classes=(MESH_OT_symmetrize_reference_preview,VIEW3D_PT_edit_symmetry_reference_preview,SculptSymmetryReferencePreview,SCULPT_OT_symref_edit_preview,SCULPT_OT_symref_finish_preview,SCULPT_OT_symref_cursor_preview,SCULPT_OT_symref_anchor_preview,SCULPT_OT_symref_detach_preview,SCULPT_OT_symref_place_preview,SCULPT_OT_symref_reset_preview,VIEW3D_GGT_sculpt_symref_preview)
def register():
    global _handle
    for cls in classes:bpy.utils.register_class(cls)
    bpy.types.Object.sculpt_symref_preview=PointerProperty(type=SculptSymmetryReferencePreview)
    import importlib.util,sys
    from pathlib import Path
    spec=importlib.util.spec_from_file_location('symmetry_shape_v2',Path(__file__).with_name('symmetry_shape_v2.py'))
    module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
    global _shape_class
    _shape_class=module.install(sys.modules[__name__])
    spec=importlib.util.spec_from_file_location('reference_hotkeys_v2',Path(__file__).with_name('reference_hotkeys_v2.py'))
    module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
    global _hotkey_class
    _hotkey_class=module.install(sys.modules[__name__])
    spec=importlib.util.spec_from_file_location('reference_native_snap',Path(__file__).with_name('reference_native_snap.py'))
    module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
    global _snap_classes
    _snap_classes=module.install(sys.modules[__name__])
    spec=importlib.util.spec_from_file_location('symmetry_multires',Path(__file__).with_name('symmetry_multires.py'))
    module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
    global _multires_class
    _multires_class=module.install(sys.modules[__name__])
    spec=importlib.util.spec_from_file_location('masked_remesh',Path(__file__).with_name('masked_remesh.py'))
    module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
    global _masked_remesh_module
    _masked_remesh_module=module
    module.install()
    bpy.utils.register_tool(SculptSymmetryReferenceTool,separator=True)
    bpy.utils.register_tool(EditSymmetryReferenceTool,separator=True)
    from bl_ui import space_view3d_toolbar
    for name in ('VIEW3D_PT_sculpt_symmetry','VIEW3D_PT_sculpt_symmetry_for_topbar'):
        panel=getattr(space_view3d_toolbar,name);panel.append(draw_panel);_panels.append(panel)
    _handle=bpy.types.SpaceView3D.draw_handler_add(draw_planes,(),'WINDOW','POST_VIEW')
    if invalidate_anchor_cache not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(invalidate_anchor_cache)
    print('SYMMETRY_REFERENCE_PREVIEW_REGISTERED',flush=True)

def unregister():
    global _handle,_session
    clear_modal_reference_preview()
    if invalidate_anchor_cache in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(invalidate_anchor_cache)
    if _handle:bpy.types.SpaceView3D.draw_handler_remove(_handle,'WINDOW');_handle=None
    for panel in _panels:panel.remove(draw_panel)
    _panels.clear()
    # Blender can tear down key-config collections before startup modules during
    # background shutdown. In that state unregister_tool raises SystemError even
    # though the workspace tools are already being destroyed.
    for tool in (EditSymmetryReferenceTool, SculptSymmetryReferenceTool):
        try:bpy.utils.unregister_tool(tool)
        except (KeyError, RuntimeError, SystemError):pass
    _masked_remesh_module.uninstall()
    bpy.utils.unregister_class(_multires_class)
    for cls in reversed(_snap_classes):bpy.utils.unregister_class(cls)
    bpy.utils.unregister_class(_hotkey_class)
    bpy.utils.unregister_class(_shape_class)
    del bpy.types.Object.sculpt_symref_preview
    for cls in reversed(classes):bpy.utils.unregister_class(cls)
    _session=None
    _last_anchor.clear()

if __name__=='__main__':register()
