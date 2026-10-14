"""Convenience UI for the existing native Multires and Sculpt Layers RNA."""
import bpy

bl_info = {
    "name": "Multires Sculpt Sidebar",
    "author": "Sculpt Extended",
    "version": (0, 1, 1),
    "blender": (5, 2, 0),
    "location": "View3D > Sidebar > Multires",
    "description": "Native Multires levels and Sculpt Layers in the N sidebar",
    "category": "3D View",
}


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
        layout.operator_context = 'INVOKE_DEFAULT'
        modifiers = multires_modifiers(context)
        if not modifiers:
            layout.label(text="Aggiungi un modificatore Multires", icon='INFO')
            return
        for modifier in modifiers:
            box = layout.box()
            box.context_pointer_set("modifier", modifier)
            row = box.row(align=True)
            row.label(text=modifier.name, icon='MOD_MULTIRES')
            row.prop(modifier, "show_viewport", text="")
            column = box.column(align=True)
            column.enabled = context.object.mode != 'EDIT'
            column.prop(modifier, "levels", text="Viewport")
            column.prop(modifier, "sculpt_levels", text="Sculpt")
            column.prop(modifier, "render_levels", text="Render")
            column.label(text="Livelli totali: " + str(modifier.total_levels))
            row = column.row(align=True)
            native_operator(row, "object.multires_subdivide", "Subdivide",
                            modifier=modifier.name, mode='CATMULL_CLARK')
            native_operator(row, "object.multires_higher_levels_delete", "Delete Higher",
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

    @classmethod
    def poll(cls, context):
        return bool(multires_modifiers(context))

    def draw(self, context):
        layout = self.layout
        layout.operator_context = 'INVOKE_DEFAULT'
        modifiers = multires_modifiers(context)
        # Native Sculpt operators target the SculptSession's modifier. RNA has
        # no public selector for that pointer; do not guess in ambiguous stacks.
        if len(modifiers) != 1:
            layout.label(text="Più Multires: usa il pannello Modificatori", icon='INFO')
            return
        modifier = modifiers[0]
        if not hasattr(modifier, "sculpt_layers"):
            layout.label(text="Questa build non espone Sculpt Layers", icon='INFO')
            return
        layout.context_pointer_set("modifier", modifier)
        layout.enabled = context.object.mode != 'EDIT'
        active_index = modifier.active_sculpt_layer_index
        for index, layer in enumerate(modifier.sculpt_layers):
            row = layout.row(align=True)
            selection = row.row(align=True)
            selection.enabled = context.object.mode == 'SCULPT'
            native_operator(selection, "sculpt.multires_layer_set_active", "",
                            'RADIOBUT_ON' if index == active_index else 'RADIOBUT_OFF',
                            index=index)
            row.prop(layer, "visible", text="")
            row.prop(layer, "name", text="")
            row.prop(layer, "strength", text="")
        if not len(modifier.sculpt_layers):
            layout.label(text="Nessun layer")
        elif 0 <= active_index < len(modifier.sculpt_layers):
            layout.label(text="Attivo: " + modifier.sculpt_layers[active_index].name)
        controls = layout.column(align=True)
        controls.enabled = context.object.mode == 'SCULPT'
        rec = controls.row()
        rec.enabled = bool(len(modifier.sculpt_layers)) and not modifier.use_sculpt_base_mesh
        rec.prop(modifier, "sculpt_layer_recording", text="REC")
        row = controls.row(align=True)
        native_operator(row, "sculpt.multires_layer_add", "Add", 'ADD')
        native_operator(row, "sculpt.multires_layer_duplicate", "Duplicate", 'DUPLICATE')
        native_operator(row, "sculpt.multires_layer_delete", "Delete", 'X')
        row = controls.row(align=True)
        native_operator(row, "sculpt.multires_layer_move", "Up", 'TRIA_UP', direction='UP')
        native_operator(row, "sculpt.multires_layer_move", "Down", 'TRIA_DOWN', direction='DOWN')
        native_operator(controls, "sculpt.multires_layer_commit", "Commit Pending Changes")
        native_operator(controls, "sculpt.multires_layer_snapshot", "Create Hidden Copy", 'DUPLICATE')
        # The new native merge appears only after its independent source patch
        # is installed. Do not alias the older bake operator: different semantics.
        native_operator(controls, "sculpt.multires_merge_visible_to_base", "Merge Visible into Base")


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

    @classmethod
    def poll(cls, context):
        mods = multires_modifiers(context)
        return len(mods) == 1 and hasattr(mods[0], 'polypaint_attribute')

    def draw(self, context):
        layout = self.layout
        layout.operator_context = 'EXEC_DEFAULT'
        modifier = multires_modifiers(context)[0]
        mesh = context.object.data
        active = mesh.color_attributes.active_color
        levels = {a.name: a.level for a in modifier.polypaint_attributes}
        if modifier.polypaint_attribute:
            levels[modifier.polypaint_attribute] = modifier.polypaint_level
        controls = layout.column()
        controls.enabled = context.object.mode in {'OBJECT', 'SCULPT'}
        for attribute in mesh.color_attributes:
            row = controls.row(align=True)
            selected = active is not None and attribute.name == active.name
            button = row.operator('sculpt.multires_polypaint_attribute', text=attribute.name,
                                  icon='RADIOBUT_ON' if selected else 'RADIOBUT_OFF', depress=selected)
            button.action = 'SELECT'
            button.name = attribute.name
            level = levels.get(attribute.name, 0)
            row.label(text=('L' + str(level)) if level else 'Base')
        row = controls.row(align=True)
        row.operator_context = 'INVOKE_DEFAULT'
        button = row.operator('multires.polypaint_manage', text='Add', icon='ADD')
        button.action = 'ADD'
        if active:
            button = row.operator('multires.polypaint_manage', text='Rename')
            button.action = 'RENAME'
            button.attribute_name = active.name
            button = row.operator('multires.polypaint_manage', text='', icon='X')
            button.action = 'REMOVE'
            button.attribute_name = active.name


classes = (VIEW3D_PT_sculpt_multires_sidebar, VIEW3D_PT_sculpt_multires_layers_sidebar,
           MULTIRES_OT_polypaint_manage, VIEW3D_PT_multires_polypaint_attributes)


_registered_here = []


def register():
    for cls in classes:
        if not hasattr(bpy.types, cls.__name__):
            bpy.utils.register_class(cls)
            _registered_here.append(cls)


def unregister():
    for cls in reversed(_registered_here):
        bpy.utils.unregister_class(cls)
    _registered_here.clear()


if __name__ == "__main__":
    register()
