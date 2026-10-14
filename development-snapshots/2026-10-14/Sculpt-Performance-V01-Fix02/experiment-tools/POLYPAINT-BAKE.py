"""UI for the native direct Polypaint UV bake. Requires the accompanying C++ patch."""
import bpy
from bpy.props import EnumProperty, IntProperty, StringProperty


def native_available():
    return bpy.ops.object.bake_image.get_rna_type().properties.get('polypaint') is not None


class OBJECT_OT_polypaint_bake_direct(bpy.types.Operator):
    bl_idname = 'object.polypaint_bake_direct'
    bl_label = 'Bake Polypaint to Texture'
    bl_description = 'Bake the color master into base UVs without creating a high resolution copy'

    resolution: EnumProperty(name='Resolution', items=[(str(n), str(n), '') for n in (512, 1024, 2048, 4096, 8192)], default='2048')
    margin: IntProperty(name='Margin', default=16, min=0, max=256)
    image_name: StringProperty(name='Existing Image', description='Optional float image; for UDIM choose an existing tiled float image', default='')

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return bool(native_available() and obj and obj.type == 'MESH' and context.mode in {'OBJECT', 'SCULPT'})

    def draw(self, context):
        layout = self.layout
        layout.prop_search(self, 'image_name', bpy.data, 'images')
        layout.prop(self, 'resolution')
        layout.prop(self, 'margin')
        layout.label(text='Empty Image: creates a new float atlas. UVs required.')
        layout.label(text='UDIM: choose an existing tiled float image.')
        layout.label(text='Bakes RGB; painted texels have alpha 1.')

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=430)

    def execute(self, context):
        obj = context.active_object
        mesh = obj.data
        if not mesh.uv_layers.active:
            self.report({'ERROR'}, 'The base mesh needs an active UV map')
            return {'CANCELLED'}
        if not any(mod.type == 'MULTIRES' and mod.total_levels > 0 for mod in obj.modifiers):
            self.report({'ERROR'}, 'A subdivided Multires modifier is required')
            return {'CANCELLED'}
        index = mesh.color_attributes.active_color_index
        if index < 0 or index >= len(mesh.color_attributes):
            self.report({'ERROR'}, 'Choose an active Color Attribute')
            return {'CANCELLED'}
        image = bpy.data.images.get(self.image_name) if self.image_name else None
        if self.image_name and image is None:
            self.report({'ERROR'}, 'The chosen image does not exist')
            return {'CANCELLED'}
        if image and not image.is_float:
            self.report({'ERROR'}, 'Choose a float image, or leave Image empty to create one')
            return {'CANCELLED'}
        created_image = image is None
        if created_image:
            size = int(self.resolution)
            image = bpy.data.images.new(obj.name+'_Polypaint', width=size, height=size, alpha=True, float_buffer=True)
            image.generated_color = (0.0, 0.0, 0.0, 0.0)

        # One image target per material used by the base mesh. No material links
        # are changed and no high-resolution object/modifier is copied.
        created_nodes = []
        created_materials = []
        saved_nodes = []
        saved_use_nodes = []
        appended_material = False
        filled_slots = []
        selected = list(context.selected_objects)
        active = context.view_layer.objects.active
        saved_margin = context.scene.render.bake.margin
        saved_clear = context.scene.render.bake.use_clear
        started = False
        try:
            if not obj.material_slots:
                material = bpy.data.materials.new(obj.name+'_PolypaintBake')
                material.use_nodes = True
                mesh.materials.append(material)
                created_materials.append(material)
                appended_material = True
            seen = set()
            for slot_index, slot in enumerate(obj.material_slots):
                material = slot.material
                if material is None:
                    material = bpy.data.materials.new(obj.name+'_PolypaintBake')
                    slot.material = material
                    material.use_nodes = True
                    filled_slots.append(slot_index)
                    created_materials.append(material)
                if material.as_pointer() in seen:
                    continue
                seen.add(material.as_pointer())
                saved_use_nodes.append((material, material.use_nodes))
                material.use_nodes = True
                nodes = material.node_tree.nodes
                saved_nodes.append((nodes, nodes.active, [(node, node.select) for node in nodes]))
                for node in nodes:
                    node.select = False
                node = nodes.new('ShaderNodeTexImage')
                node.name = 'Polypaint Bake Target'
                node.label = 'Polypaint UV Bake'
                node.image = image
                node.select = True
                nodes.active = node
                created_nodes.append((nodes, node))
            for other in selected:
                other.select_set(False)
            obj.select_set(True)
            context.view_layer.objects.active = obj
            context.scene.render.bake.margin = self.margin
            context.scene.render.bake.use_clear = True
            result = bpy.ops.object.bake_image('INVOKE_DEFAULT', polypaint=True)
            started = 'RUNNING_MODAL' in result or 'FINISHED' in result
            if not started:
                raise RuntimeError('Native bake did not start; see the native error report')
            print('[POLYPAINT_BAKE_UI] object='+obj.name+' image='+image.name+' base_vertices='+str(len(mesh.vertices)), flush=True)
            self.report({'INFO'}, 'Bake started: '+image.name+'. Save the image from the Image Editor after completion.')
            return {'FINISHED'}
        except Exception as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        finally:
            context.scene.render.bake.margin = saved_margin
            context.scene.render.bake.use_clear = saved_clear
            for other in context.selected_objects:
                other.select_set(False)
            for other in selected:
                other.select_set(True)
            context.view_layer.objects.active = active
            if not started:
                for nodes, node in created_nodes:
                    nodes.remove(node)
                for nodes, old_active, states in saved_nodes:
                    nodes.active = old_active
                    for node, state in states:
                        node.select = state
                for material, use_nodes in saved_use_nodes:
                    material.use_nodes = use_nodes
                if appended_material:
                    mesh.materials.pop(index=len(mesh.materials)-1)
                for slot_index in filled_slots:
                    obj.material_slots[slot_index].material = None
                for material in created_materials:
                    if material.users == 0:
                        bpy.data.materials.remove(material)
                if created_image and image.users == 0:
                    bpy.data.images.remove(image)


class VIEW3D_PT_polypaint_bake_direct(bpy.types.Panel):
    bl_label = 'Polypaint Texture Bake'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Polypaint'

    def draw(self, context):
        self.layout.operator('object.polypaint_bake_direct', text='Bake Polypaint to Texture', icon='RENDER_STILL')
        self.layout.label(text='Base UVs; original master preserved.')


def register():
    if not native_available():
        print('[POLYPAINT_BAKE_UI] native patch missing; UI not registered', flush=True)
        return
    for cls in (OBJECT_OT_polypaint_bake_direct, VIEW3D_PT_polypaint_bake_direct):
        old = getattr(bpy.types, cls.__name__, None)
        if old:
            bpy.utils.unregister_class(old)
        bpy.utils.register_class(cls)


register()
