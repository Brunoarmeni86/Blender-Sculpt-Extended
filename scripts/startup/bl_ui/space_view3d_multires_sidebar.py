"""Convenience UI for the existing native Multires and Sculpt Layers RNA."""
import bpy




def multires_modifiers(context):
    obj = context.object
    if obj is None or obj.type != 'MESH':
        return []
    return [modifier for modifier in obj.modifiers if modifier.type == 'MULTIRES']


def native_operator(layout, identifier, text, icon='NONE', **properties):
    """Expose only registered operators; never synthesize a replacement."""
    namespace, name = identifier.split('.', 1)
    try:
        operator = getattr(getattr(bpy.ops, namespace), name)
        operator.get_rna_type()
    except (AttributeError, RuntimeError):
        return None
    pointer = layout.operator(identifier, text=text, icon=icon)
    for key, value in properties.items():
        setattr(pointer, key, value)
    return pointer


class SCULPT_OT_multires_layer_strength_deferred(bpy.types.Operator):
    """Edit the numeric value freely, then evaluate dense Multires only once."""
    bl_idname = "sculpt.multires_layer_strength_deferred"
    bl_label = "Set Layer Strength"
    bl_description = "Adjust Strength without rebuilding Multires until you confirm"
    bl_options = {'REGISTER'}

    modifier_name: bpy.props.StringProperty(options={'HIDDEN'})
    layer_index: bpy.props.IntProperty(options={'HIDDEN'})
    strength: bpy.props.FloatProperty(name="Strength", default=1.0, min=-10.0, max=10.0)

    def _layer(self, context):
        obj = context.object
        modifier = obj.modifiers.get(self.modifier_name) if obj else None
        if modifier is None or not hasattr(modifier, 'sculpt_layers'):
            return None
        if not 0 <= self.layer_index < len(modifier.sculpt_layers):
            return None
        return modifier.sculpt_layers[self.layer_index]

    def invoke(self, context, event):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        self.strength = layer.strength
        return context.window_manager.invoke_props_dialog(self, width=320)

    def draw(self, context):
        column = self.layout.column()
        column.prop(self, 'strength', slider=True)
        column.label(text="The surface is updated only after you confirm", icon='INFO')

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        if abs(layer.strength - self.strength) <= 1.0e-12:
            return {'CANCELLED'}
        layer.strength = self.strength
        return {'FINISHED'}


class VIEW3D_PT_sculpt_multires_sidebar(bpy.types.Panel):
    bl_idname = "VIEW3D_PT_sculpt_multires_sidebar"
    bl_label = "Multiresolution"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Multires"

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.type == 'MESH'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.operator_context = 'INVOKE_DEFAULT'
        modifiers = multires_modifiers(context)
        if not modifiers:
            layout.label(text="Add a Multires Modifier", icon='INFO')
            return
        for modifier in modifiers:
            column = layout.column(align=True)
            column.context_pointer_set("modifier", modifier)
            row = column.row(align=True)
            row.label(text=modifier.name, icon='MOD_MULTIRES')
            row.prop(modifier, "show_viewport", text="", icon='RESTRICT_VIEW_OFF')
            column.enabled = context.object.mode != 'EDIT'
            column.prop(modifier, "levels", text="Viewport")
            column.prop(modifier, "sculpt_levels", text="Sculpt")
            column.prop(modifier, "render_levels", text="Render")
            column.label(text="Total Levels: " + str(modifier.total_levels), icon='INFO')
            row = column.row(align=True)
            native_operator(row, "object.multires_subdivide", "Subdivide",
                            modifier=modifier.name, mode='CATMULL_CLARK')
            native_operator(row, "object.multires_higher_levels_delete", "Delete Higher", 'TRASH',
                            modifier=modifier.name)
            if hasattr(modifier, "use_sculpt_base_mesh"):
                row = column.row()
                row.enabled = context.object.mode == 'SCULPT'
                row.prop(modifier, "use_sculpt_base_mesh", text="Sculpt Base Mesh")


class VIEW3D_PT_sculpt_multires_layers_sidebar(bpy.types.Panel):
    bl_idname = "VIEW3D_PT_sculpt_multires_layers_sidebar"
    bl_parent_id = "VIEW3D_PT_sculpt_multires_sidebar"
    bl_label = "Sculpt Layers"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Multires"
    bl_order = 0

    @classmethod
    def poll(cls, context):
        return bool(multires_modifiers(context))

    def draw(self, context):
        layout = self.layout
        layout.use_property_decorate = False
        layout.operator_context = 'INVOKE_DEFAULT'
        modifiers = multires_modifiers(context)
        # Native Sculpt operators target the SculptSession's modifier. RNA has
        # no public selector for that pointer; do not guess in ambiguous stacks.
        if len(modifiers) != 1:
            layout.label(text="Multiple Multires modifiers: use the Modifiers panel", icon='INFO')
            return
        modifier = modifiers[0]
        if not hasattr(modifier, "sculpt_layers"):
            layout.label(text="This build does not expose Sculpt Layers", icon='INFO')
            return
        layout.context_pointer_set("modifier", modifier)
        layout.enabled = context.object.mode != 'EDIT'
        active_index = modifier.active_sculpt_layer_index
        row = layout.row()
        layer_list = row.box().column(align=True)
        last_group = 0
        for index, layer in enumerate(modifier.sculpt_layers):
            group_id = getattr(layer, 'group_id', 0)
            if group_id and group_id != last_group:
                group = layer_list.row(align=True)
                op = group.operator(
                    'sculpt.multires_layer_group_collapse', text='', emboss=False,
                    icon='TRIA_RIGHT' if layer.group_collapsed else 'TRIA_DOWN')
                op.group_id = group_id
                op = group.operator(
                    'sculpt.multires_layer_group_rename',
                    text=layer.group_name or 'Group', icon='FILE_FOLDER')
                op.group_id = group_id
                op.name = layer.group_name or 'Group'
                op = group.operator('sculpt.multires_layer_group_move', text='',
                                    icon='TRIA_UP', emboss=False)
                op.group_id = group_id
                op.direction = 'UP'
                op = group.operator('sculpt.multires_layer_group_move', text='',
                                    icon='TRIA_DOWN', emboss=False)
                op.group_id = group_id
                op.direction = 'DOWN'
                op = group.operator(
                    'sculpt.multires_layer_group_visibility', text='', emboss=False,
                    icon='HIDE_ON' if layer.group_hidden else 'HIDE_OFF')
                op.group_id = group_id
                op = group.operator('sculpt.multires_layer_group_add_active', text='',
                                    icon='ADD', emboss=False)
                op.group_id = group_id
                op = group.operator('sculpt.multires_layer_group_dissolve', text='',
                                    icon='X', emboss=False)
                op.group_id = group_id
                op = group.operator('sculpt.multires_layer_group_delete', text='',
                                    icon='TRASH', emboss=False)
                op.group_id = group_id
            last_group = group_id
            if group_id and layer.group_collapsed:
                continue
            item = layer_list.row(align=True)
            if group_id:
                item.separator(factor=1.0)
            op = item.operator(
                'sculpt.multires_layer_set_active', text='', emboss=False,
                icon='RADIOBUT_ON' if index == active_index else 'RADIOBUT_OFF')
            op.index = index
            item.prop(layer, 'name', text='')
            # Keep the native field available and add a deferred editor. The
            # latter avoids evaluating a dense 122M-sample layer for every
            # mouse-move event; exact RNA evaluation happens once on OK.
            strength = item.row(align=True)
            strength.prop(layer, 'strength', text='')
            deferred = strength.operator(
                'sculpt.multires_layer_strength_deferred', text='', icon='TIME')
            deferred.modifier_name = modifier.name
            deferred.layer_index = index
            item.prop(layer, 'visible', text='', emboss=False,
                      icon='HIDE_OFF' if layer.visible else 'HIDE_ON')
            if group_id:
                op = item.operator('sculpt.multires_layer_group_remove', text='',
                                   icon='X', emboss=False)
                op.index = index
        if not modifier.sculpt_layers:
            layer_list.label(text='No Sculpt Layers', icon='INFO')
        side = row.column(align=True)
        side.enabled = context.object.mode == 'SCULPT'
        native_operator(side, "sculpt.multires_layer_add", "", 'ADD')
        native_operator(side, "sculpt.multires_layer_delete", "", 'REMOVE')
        side.separator()
        native_operator(side, "sculpt.multires_layer_duplicate", "", 'DUPLICATE')
        side.separator()
        native_operator(side, "sculpt.multires_layer_move", "", 'TRIA_UP', direction='UP')
        native_operator(side, "sculpt.multires_layer_move", "", 'TRIA_DOWN', direction='DOWN')
        side.separator()
        native_operator(side, 'sculpt.multires_layer_group_create', '', 'FILE_FOLDER')
        native_operator(side, 'sculpt.multires_layer_group_remove', '', 'REMOVE')
        controls = layout.column(align=True)
        controls.enabled = context.object.mode == 'SCULPT'
        native_operator(controls, "sculpt.multires_layer_commit", "Commit Pending Changes")
        native_operator(controls, "sculpt.multires_layer_snapshot", "Create Hidden Copy", 'DUPLICATE')
        bake = controls.column(align=True)
        bake.operator_context = 'INVOKE_REGION_WIN'
        native_operator(bake, 'sculpt.multires_base_capture', 'Bake Visible into Base...')
        # The new native merge appears only after its independent source patch
        # is installed. Do not alias the older bake operator: different semantics.
        # Merge is an Object Mode transaction, independent of Sculpt controls.
        merge = layout.column(align=True)
        merge.enabled = context.object.mode == 'OBJECT'
        native_operator(merge, "sculpt.multires_merge_visible_to_base", "Merge Visible into Base")
        if context.object.mode != 'OBJECT':
            layout.label(text="Merge Visible: switch to Object Mode", icon='INFO')


class MULTIRES_OT_polypaint_manage(bpy.types.Operator):
    bl_idname = "multires.polypaint_manage"
    bl_label = "Color Attribute"
    # The native operator records Undo. This UI wrapper must not add a second step.
    action: bpy.props.EnumProperty(items=[('ADD', 'Add', ''), ('REMOVE', 'Remove', ''), ('RENAME', 'Rename', '')])
    attribute_name: bpy.props.StringProperty(options={'HIDDEN'})
    new_name: bpy.props.StringProperty(name="Name", default="Color", maxlen=67)

    def invoke(self, context, event):
        if self.action == 'REMOVE':
            return self.execute(context)
        if self.action == 'RENAME':
            self.new_name = self.attribute_name
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        self.layout.prop(self, "new_name")

    def execute(self, context):
        value = self.new_name.strip() if self.action == 'ADD' else self.attribute_name
        try:
            result = bpy.ops.sculpt.multires_polypaint_attribute(
                'EXEC_DEFAULT', True, action=self.action, name=value,
                new_name=self.new_name.strip() if self.action == 'RENAME' else '')
        except RuntimeError as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        return result


class VIEW3D_PT_multires_polypaint_attributes(bpy.types.Panel):
    bl_idname = "VIEW3D_PT_multires_polypaint_attributes"
    bl_parent_id = "VIEW3D_PT_sculpt_multires_sidebar"
    bl_label = "Polypaint Attributes"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Multires"
    bl_order = 20

    @classmethod
    def poll(cls, context):
        mods = multires_modifiers(context)
        return len(mods) == 1 and hasattr(mods[0], 'polypaint_attribute')

    def draw(self, context):
        layout = self.layout
        layout.use_property_decorate = False
        layout.operator_context = 'EXEC_DEFAULT'
        modifier = multires_modifiers(context)[0]
        mesh = context.object.data
        active = mesh.color_attributes.active_color
        levels = {a.name: a.level for a in modifier.polypaint_attributes}
        if modifier.polypaint_attribute:
            levels[modifier.polypaint_attribute] = modifier.polypaint_level
        controls = layout.column()
        controls.enabled = context.object.mode in {'OBJECT', 'SCULPT'}
        attribute_box = controls.box()
        for attribute in mesh.color_attributes:
            row = attribute_box.row(align=True)
            selected = active is not None and attribute.name == active.name
            button = row.operator('sculpt.multires_polypaint_attribute', text=attribute.name,
                                  icon='RADIOBUT_ON' if selected else 'RADIOBUT_OFF', depress=selected)
            button.action = 'SELECT'
            button.name = attribute.name
            level = levels.get(attribute.name, 0)
            row.label(text=('L' + str(level)) if level else 'Base')
        if not mesh.color_attributes:
            attribute_box.label(text='No Color Attributes', icon='INFO')
        row = controls.row(align=True)
        row.operator_context = 'INVOKE_DEFAULT'
        row.operator('sculpt.color_attribute_add_auto_v07', text='Add', icon='ADD')
        if active:
            button = row.operator('multires.polypaint_manage', text='Rename')
            button.action = 'RENAME'
            button.attribute_name = active.name
            button = row.operator('multires.polypaint_manage', text='', icon='X')
            button.action = 'REMOVE'
            button.attribute_name = active.name


classes = (SCULPT_OT_multires_layer_strength_deferred,
           VIEW3D_PT_sculpt_multires_sidebar, VIEW3D_PT_sculpt_multires_layers_sidebar,
           MULTIRES_OT_polypaint_manage, VIEW3D_PT_multires_polypaint_attributes)
