"""Native Blender smoke regression for the subpixel UV coverage gap.

Run only in a fresh background process, with --factory-startup and --python.
This file has not been executed here: Blender is not installed in this environment.
It checks raster coverage, not sculpt reconstruction accuracy or native TBN error.
"""
import argparse
import json
import math
import os
import pathlib
import sys

import bpy


def main():
    if not bpy.app.background:
        raise RuntimeError("Run this test in background Blender with --factory-startup.")
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    mesh = bpy.data.meshes.new("CoverageQuad")
    mesh.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
    mesh.update()
    mesh.polygons[0].use_smooth = True
    ob = bpy.data.objects.new("CoverageQuad", mesh)
    bpy.context.collection.objects.link(ob)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob

    # The whole island is inside the first texel's lower-left corner. No texel
    # center lies in it, but the 4x4 sample at (0.125,0.125) is covered.
    uv = mesh.uv_layers.new(name="UVMap")
    lo, hi = .03125 / 8, .40625 / 8
    flat_uv = [lo, lo, hi, lo, hi, hi, lo, hi]
    if hasattr(uv, "uv"):
        uv.uv.foreach_set("vector", flat_uv)
    else:
        uv.data.foreach_set("uv", flat_uv)

    mmd = ob.modifiers.new("Multires", "MULTIRES")
    for _ in range(2):
        bpy.ops.object.multires_subdivide(modifier=mmd.name)
    mmd.levels = 0
    mmd.render_levels = 2
    material = bpy.data.materials.new("CoverageMaterial")
    material.use_nodes = True
    mesh.materials.append(material)
    node = material.node_tree.nodes.new("ShaderNodeTexImage")
    material.node_tree.nodes.active = node
    node.select = True

    settings = bpy.context.scene.render.bake
    settings.use_multires = True
    settings.use_clear = True
    settings.margin = 0
    expected_alpha = 0.0 if os.environ.get("BLENDER_MULTIRES_BAKE_UV_COVERAGE") == "0" else 1.0
    results = []
    for bake_type, space, lores in (
        ("NORMALS", "OBJECT", True),
        ("VECTOR_DISPLACEMENT", "OBJECT", True),
        ("VECTOR_DISPLACEMENT", "TANGENT", True),
        ("VECTOR_DISPLACEMENT", "OBJECT", False),
        ("VECTOR_DISPLACEMENT", "TANGENT", False),
    ):
        image = bpy.data.images.new(f"{bake_type}_{space}_{lores}", width=8, height=8,
                                    alpha=True, float_buffer=True)
        image.alpha_mode = "STRAIGHT"
        image.colorspace_settings.name = "Non-Color"
        node.image = image
        settings.type = bake_type
        settings.displacement_space = space
        settings.use_lores_mesh = lores
        status = bpy.ops.object.bake_image("EXEC_DEFAULT")
        if status != {"FINISHED"}:
            raise AssertionError(f"Bake failed: {bake_type}, {space}, {lores}: {status}")
        pixel = list(image.pixels[:4])
        if not all(math.isfinite(value) for value in pixel):
            raise AssertionError(f"Non-finite pixel: {pixel}")
        if abs(pixel[3] - expected_alpha) > 1e-6:
            raise AssertionError(f"Expected alpha {expected_alpha}, got {pixel}; type={bake_type}, space={space}, lores={lores}")
        results.append(dict(type=bake_type, space=space, lores=lores, pixel=pixel))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(dict(blender=bpy.app.version_string,
                                        coverage=os.environ.get("BLENDER_MULTIRES_BAKE_UV_COVERAGE", "1"),
                                        results=results), indent=2))
    print(f"NATIVE_COVERAGE_PASS: {len(results)} cases, expected alpha={expected_alpha}; {args.output}")


if __name__ == "__main__":
    main()
