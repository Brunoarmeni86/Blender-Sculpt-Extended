import bpy
import os


bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=4, radius=2.0)
obj = bpy.context.object
obj.name = "Mask_Remesh_Operator_Probe"
mesh = obj.data
mask = mesh.attributes.new(".sculpt_mask", "FLOAT", "POINT")
weight = mesh.attributes.new("probe_weight", "FLOAT", "POINT")
for vertex in mesh.vertices:
    mask.data[vertex.index].value = 1.0 if vertex.co.z > .9 else (
        .5 if vertex.co.z > -.3 else 0.0)
    weight.data[vertex.index].value = vertex.co.x * .17 + vertex.co.y * .31 + 2.0
mesh.remesh_voxel_size = .32
mesh.use_remesh_preserve_attributes = True
mesh.sculpt_remesh_protect_mask_preview = True
mesh.sculpt_remesh_mask_coarsening = 2.5
mesh.sculpt_remesh_mask_falloff = 1.0
bpy.ops.wm.save_as_mainfile(filepath=os.environ["SCULPT_MASK_REMESH_BASELINE"])
