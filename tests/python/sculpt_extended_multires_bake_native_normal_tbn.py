"""Native Normal bake A/B probe. Run each mode in a fresh Blender process.

The 1x1 reference already uses the raw TBN inverse. Compare reconstructed object
normals on curved, smooth triangles with regular and mirrored UV islands.
This is a center-reference diagnostic, not a Cycles render or a filter oracle.
"""
import argparse
import hashlib
import json
import math
import os
import pathlib
import sys


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def unit(v):
    length = math.sqrt(dot(v, v))
    if length < 1e-12:
        raise AssertionError("Degenerate decoded normal")
    return [x / length for x in v]


def angle(a, b):
    return math.degrees(math.acos(max(-1, min(1, dot(unit(a), unit(b))))))


def compare(folder):
    reports = [json.loads((folder / f"native-normal-{mode}.json").read_text())
               for mode in ("reference", "previous", "corrected")]
    reference, previous, corrected = reports
    if [r["ss"] for r in reports] != ["1", "4", "4"]:
        raise AssertionError("Unexpected sampling modes")
    if [r["cycles_tbn"] for r in reports[1:]] != ["0", "1"]:
        raise AssertionError("Unexpected TBN modes")
    if len({r["frame_hash"] for r in reports}) != 1:
        raise AssertionError("Scene frames changed between processes")
    old_errors, new_errors, changes = [], [], []
    for a, b, c in zip(*(r["samples"] for r in reports)):
        if a["pixel"] != b["pixel"] or a["pixel"] != c["pixel"]:
            raise AssertionError("Probe pixels changed")
        old_errors.append(angle(a["object_normal"], b["object_normal"]))
        new_errors.append(angle(a["object_normal"], c["object_normal"]))
        changes.append(angle(b["object_normal"], c["object_normal"]))
    if not changes or max(changes) < 1e-5:
        raise AssertionError("TBN switch produced no measurable change; check compiled binary")
    def stats(values):
        return dict(rms_degrees=math.sqrt(sum(x*x for x in values)/len(values)),
                    max_degrees=max(values))
    result = dict(samples=len(changes), previous_vs_1x=stats(old_errors),
                  corrected_vs_1x=stats(new_errors), previous_vs_corrected=stats(changes),
                  corrected_closer_to_center_reference=sum(new_errors) < sum(old_errors),
                  scope="Native bake and Cycles-equation decode; 1x center vs 4x4 filtered samples. Not a render correctness assertion.")
    (folder / "native-normal-comparison.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


def bake(output):
    import bpy
    if not bpy.app.background:
        raise RuntimeError("Run in background Blender")
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=1, radius=1, calc_uvs=False)
    ob = bpy.context.object
    mesh = ob.data
    # Non-spherical curvature makes the frame defect measurable.
    for v in mesh.vertices:
        v.co.x *= 1.6
        v.co.y *= .8
        v.co.z *= 1.2
    mesh.update()
    for poly in mesh.polygons:
        poly.use_smooth = True
    uv = mesh.uv_layers.new(name="TBNProbe")
    # Bake reads the active mesh UV map, not the uvmap argument of calc_tangents.
    uv.active = True
    uv.active_render = True
    if mesh.uv_layers.active.name != uv.name:
        raise AssertionError("Probe UV map is not active")
    flat_uv = [0.0] * (2 * len(mesh.loops))
    triangles = []
    for i, poly in enumerate(mesh.polygons):
        if len(poly.loop_indices) != 3:
            raise AssertionError("Probe requires original triangular faces")
        x, y = i % 5, i // 5
        coords = [((x+.12)/5, (y+.12)/4), ((x+.88)/5, (y+.12)/4),
                  ((x+.12)/5, (y+.88)/4)]
        if i % 2:
            coords[1], coords[2] = coords[2], coords[1]
        for loop, coord in zip(poly.loop_indices, coords):
            flat_uv[2*loop:2*loop+2] = coord
        triangles.append((list(poly.loop_indices), coords))
    if hasattr(uv, "uv"):
        uv.uv.foreach_set("vector", flat_uv)
    else:
        uv.data.foreach_set("uv", flat_uv)
    mesh.calc_tangents(uvmap=uv.name)
    frames = []
    for loops, coords in triangles:
        frames.append(dict(uv=coords,
            normals=[list(mesh.vertices[mesh.loops[i].vertex_index].normal) for i in loops],
            tangents=[list(mesh.loops[i].tangent) for i in loops],
            signs=[mesh.loops[i].bitangent_sign for i in loops]))
    mmd = ob.modifiers.new("Multires", "MULTIRES")
    for _ in range(3):
        bpy.ops.object.multires_subdivide(modifier=mmd.name)
    mmd.levels = 0
    mmd.render_levels = 3
    if mesh.uv_layers.active.name != uv.name:
        raise AssertionError("Multires changed the active UV map")
    material = bpy.data.materials.new("NormalProbe")
    material.use_nodes = True
    mesh.materials.append(material)
    node = material.node_tree.nodes.new("ShaderNodeTexImage")
    material.node_tree.nodes.active = node
    image = bpy.data.images.new("NormalProbe", width=256, height=256, alpha=True, float_buffer=True)
    image.colorspace_settings.name = "Non-Color"
    image.alpha_mode = "STRAIGHT"
    node.image = image
    settings = bpy.context.scene.render.bake
    settings.use_multires = True
    settings.use_clear = True
    settings.margin = 0
    settings.type = "NORMALS"
    if bpy.ops.object.bake_image("EXEC_DEFAULT") != {"FINISHED"}:
        raise AssertionError("Normal bake failed")
    pixels = list(image.pixels[:])
    if not all(math.isfinite(x) for x in pixels):
        raise AssertionError("Non-finite baked image")
    samples = []
    for face, frame in enumerate(frames):
        uvs = frame["uv"]
        for weights in ((.2,.2,.6),(.2,.6,.2),(.6,.2,.2),(.34,.33,.33),(.4,.4,.2),(.4,.2,.4)):
            uvp = [sum(weights[i]*uvs[i][j] for i in range(3)) for j in range(2)]
            x,y = [int(v*256) for v in uvp]
            p = ((x+.5)/256, (y+.5)/256)
            a,b,c = uvs
            det = (b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
            w0 = ((b[1]-c[1])*(p[0]-c[0])+(c[0]-b[0])*(p[1]-c[1]))/det
            w1 = ((c[1]-a[1])*(p[0]-c[0])+(a[0]-c[0])*(p[1]-c[1]))/det
            w = (w0,w1,1-w0-w1)
            if min(w) < .1:
                raise AssertionError("Probe too close to UV boundary")
            def interp(name):
                return [sum(w[i]*frame[name][i][j] for i in range(3)) for j in range(3)]
            n,t = interp("normals"),interp("tangents")
            sign = -1 if sum(w[i]*frame["signs"][i] for i in range(3)) < 0 else 1
            b = [sign*(n[1]*t[2]-n[2]*t[1]),sign*(n[2]*t[0]-n[0]*t[2]),sign*(n[0]*t[1]-n[1]*t[0])]
            rgb = pixels[4*(y*256+x):4*(y*256+x)+4]
            if abs(rgb[3]-1) > 1e-6:
                raise AssertionError(f"Missing interior coverage: {face}, {x}, {y}, {rgb}")
            decoded = [2*v-1 for v in rgb[:3]]
            obj = unit([t[j]*decoded[0]+b[j]*decoded[1]+n[j]*decoded[2] for j in range(3)])
            samples.append(dict(face=face,pixel=[x,y],rgb=rgb,object_normal=obj))
    report = dict(blender=bpy.app.version_string,
                  ss=os.environ.get("BLENDER_MULTIRES_BAKE_SS", "unset"),
                  cycles_tbn=os.environ.get("BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN", "1"),
                  active_uv=mesh.uv_layers.active.name,
                  frame_hash=hashlib.sha256(json.dumps(frames,sort_keys=True).encode()).hexdigest(),
                  samples=samples)
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(report,indent=2))
    print(f"NORMAL_TBN_NATIVE_PROBE: {len(samples)} finite covered samples; {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output",type=pathlib.Path)
    parser.add_argument("--compare",type=pathlib.Path)
    args = parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else [])
    if args.compare:
        compare(args.compare)
    elif args.output:
        bake(args.output)
    else:
        parser.error("--output or --compare required")
