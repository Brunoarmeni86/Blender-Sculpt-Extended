"""Compare saved Blender sculpt trials; run with the experimental Blender binary.

blender -b -t 1 --python verify_blender_brush_trial.py -- \
    --before before.blend --after after.blend --undo undo.blend --object TrialMesh

No mesh edits or file saves. The input files must be separate snapshots of
the same object, saved before the stroke, after it, and optionally after Undo.
This verifies coordinates/attributes, not whether the physics backend ran:
also inspect the experimental backend's console log.
"""
import argparse
from array import array
import hashlib
import sys

import bpy


def digest(collection, prop, count, code):
    buf = array(code, [0]) * count
    collection.foreach_get(prop, buf)
    return hashlib.sha256(buf.tobytes()).hexdigest()


def snapshot(path, object_name):
    bpy.ops.wm.open_mainfile(filepath=path)
    obj = bpy.data.objects.get(object_name)
    if obj is None or obj.type != 'MESH':
        raise RuntimeError(f"Mesh object {object_name!r} missing in {path}")
    mesh = obj.data
    nv, ne, nl, nf = map(len, (mesh.vertices, mesh.edges, mesh.loops, mesh.polygons))
    topology = (
        nv, ne, nl, nf,
        digest(mesh.edges, "vertices", 2 * ne, "i"),
        digest(mesh.loops, "vertex_index", nl, "i"),
        digest(mesh.loops, "edge_index", nl, "i"),
        digest(mesh.polygons, "loop_start", nf, "i"),
        digest(mesh.polygons, "loop_total", nf, "i"),
    )
    uv = {}
    for layer in mesh.uv_layers:
        if hasattr(layer, "uv"):
            uv[layer.name] = digest(layer.uv, "vector", 2 * nl, "f")
        else:
            uv[layer.name] = digest(layer.data, "uv", 2 * nl, "f")
    if not uv:
        raise RuntimeError(f"Test requires at least one UV layer: {path}")
    colors = {
        attr.name: (attr.domain, attr.data_type,
                    digest(attr.data, "color", 4 * len(attr.data), "f"))
        for attr in mesh.color_attributes
    }
    attributes = {}
    for name in (".sculpt_mask", ".sculpt_face_set", ".hide_vert", ".hide_poly"):
        attr = mesh.attributes.get(name)
        if attr is not None:
            code = {"FLOAT": "f", "INT": "i", "BOOLEAN": "b"}.get(attr.data_type)
            if code is None:
                raise RuntimeError(f"Unexpected attribute type for {name}: {attr.data_type}")
            attributes[name] = (attr.domain, attr.data_type,
                                digest(attr.data, "value", len(attr.data), code))
    return {
        "topology": topology,
        "uv": uv,
        "uv_order": tuple(layer.name for layer in mesh.uv_layers),
        "colors": colors,
        "attributes": attributes,
        "positions": digest(mesh.vertices, "co", 3 * nv, "f"),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", required=True)
    parser.add_argument("--after", required=True)
    parser.add_argument("--undo")
    parser.add_argument("--object", required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    baseline = snapshot(args.before, args.object)
    after = snapshot(args.after, args.object)
    for key in ("topology", "uv", "uv_order", "colors", "attributes"):
        if baseline[key] != after[key]:
            raise RuntimeError(f"FAIL: {key} changed after brush stroke")
        print(f"PASS: {key} unchanged")
    if baseline["positions"] == after["positions"]:
        raise RuntimeError("FAIL: no geometry change; stroke trial is inconclusive")
    print("PASS: vertex positions changed")
    if args.undo:
        undo = snapshot(args.undo, args.object)
        for key in baseline:
            if baseline[key] != undo[key]:
                raise RuntimeError(f"FAIL: Undo did not restore {key}")
        print("PASS: Undo snapshot equals before snapshot")
    else:
        print("NOT TESTED: Undo snapshot not supplied")
    print("PASS: coordinate-only brush snapshot contract")


if __name__ == "__main__":
    main()
