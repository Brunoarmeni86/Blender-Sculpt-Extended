"""Prepare a small interactive scene only when explicitly launched with TestPolypaint."""
import bpy
import os
import sys
import traceback
import runpy
from pathlib import Path

runpy.run_path(str(Path(__file__).parent.parent / "LOAD-SIDEBAR.py"))
runpy.run_path(str(Path(__file__).with_name("LAYER-DIAGNOSTICS.py")))

print('[SCULPT_POLYPAINT_PROFILE]', {
    key: os.getenv(key) for key in (
        'BLENDER_SCULPT_COMMIT_PUBLISH_SMALL_GRIDS',
        'BLENDER_SCULPT_COMPACT_COMMIT_STAGING',
        'BLENDER_SCULPT_FIRST_STROKE_ANCHOR',
        'BLENDER_SCULPT_POLYPAINT_CPU',
    )
}, flush=True)

def prepare_scene():
    try:
        if not bpy.context.window:
            raise RuntimeError('La prova richiede Blender con una finestra e viewport.')
        scene = bpy.data.scenes.new('Polypaint Test')
        bpy.context.window.scene = scene
        bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=8)
        obj = bpy.context.object
        obj.name = 'Polypaint-Test-Sphere'
        mesh = obj.data
        attr = mesh.color_attributes.new(name='SkinColor', type='FLOAT_COLOR', domain='CORNER')
        for value in attr.data:
            value.color = (0.15, 0.35, 0.12, 1.0)
        mesh.color_attributes.active_color_index = list(mesh.color_attributes).index(attr)
        mesh.color_attributes.render_color_index = list(mesh.color_attributes).index(attr)
        mat = bpy.data.materials.new('Polypaint-Test-SkinColor')
        mat.use_nodes = True
        color = mat.node_tree.nodes.new('ShaderNodeVertexColor')
        color.layer_name = 'SkinColor'
        bsdf = next(node for node in mat.node_tree.nodes if node.type == 'BSDF_PRINCIPLED')
        mat.node_tree.links.new(color.outputs['Color'], bsdf.inputs['Base Color'])
        mesh.materials.append(mat)
        mmd = obj.modifiers.new(name='Multires', type='MULTIRES')
        for _ in range(3):
            bpy.ops.object.multires_subdivide(modifier=mmd.name, mode='CATMULL_CLARK')
        mmd.levels = mmd.sculpt_levels = 3
        for polygon in mesh.polygons:
            polygon.use_smooth = True
        bpy.ops.object.mode_set(mode='SCULPT')
        loaded = False
        for name in ('Paint Hard', 'Paint'):
            try:
                result = bpy.ops.brush.asset_activate(
                    asset_library_type='ESSENTIALS',
                    relative_asset_identifier=f'brushes/essentials_brushes-mesh_sculpt.blend/Brush/{name}')
                if 'FINISHED' in result:
                    brush = scene.tool_settings.sculpt.brush
                    if brush and brush.sculpt_brush_type == 'PAINT':
                        brush.color = (0.85, 0.02, 0.01)
                        brush.secondary_color = (0.02, 0.1, 0.85)
                        brush.strength = 0.8
                        loaded = True
                        break
            except Exception as exc:
                print('[POLYPAINT_TEST_BRUSH]', name, str(exc), flush=True)
        for screen in bpy.data.screens:
            for area in screen.areas:
                if area.type == 'VIEW_3D':
                    area.spaces.active.shading.color_type = 'VERTEX'
                    area.spaces.active.region_3d.view_distance = 4.0
                    area.tag_redraw()
        print('[POLYPAINT_TEST_READY] level=3 attribute=SkinColor brush_loaded=' + str(loaded), flush=True)
        print('Paint rosso -> Undo -> Redo. Sculpt level 3 -> 1 -> 3; poi salva e riapri il file di test.', flush=True)
        if not loaded:
            print('Seleziona manualmente il pennello Paint dalla libreria Essential.', flush=True)
    except Exception:
        print('[POLYPAINT_TEST_SETUP_FAILED]', flush=True)
        traceback.print_exc()
    return None

if '--polypaint-test' in sys.argv:
    bpy.app.timers.register(prepare_scene, first_interval=1.0)

if bpy.ops.object.bake_image.get_rna_type().properties.get("polypaint") is not None:
    runpy.run_path(str(Path(__file__).with_name("POLYPAINT-BAKE.py")))
