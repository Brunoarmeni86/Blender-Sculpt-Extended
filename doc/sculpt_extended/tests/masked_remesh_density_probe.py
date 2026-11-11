"""Diagnostic coverage for density reduction and mask-gradient transitions."""

import importlib.util
from pathlib import Path

import bmesh
import bpy


ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "scripts" / "startup" / "sculpt_extended_integrated" / "masked_remesh.py"
spec = importlib.util.spec_from_file_location("masked_remesh_density_probe_module", MODULE)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def run(voxel):
    mesh = bpy.data.meshes.new(f"__density_probe_{voxel}")
    seed = bmesh.new()
    result = None
    try:
        bmesh.ops.create_icosphere(seed, subdivisions=5, radius=2.0)
        seed.to_mesh(mesh)
        initial = (len(mesh.vertices), len(mesh.polygons))
        mask_attr = mesh.attributes.new(".sculpt_mask", "FLOAT", "POINT")
        for vertex in mesh.vertices:
            # Editable lower hemisphere, smooth transition, immutable top cap.
            mask_attr.data[vertex.index].value = max(0.0, min(1.0, (vertex.co.z + 0.8) / 1.6))

        result, info = mod.local_remesh(mesh, voxel, coarsening=3.0, falloff=1.0)
        mask = result.verts.layers.float[".sculpt_mask"]
        bands = {"low": [], "mid": [], "high": []}
        for edge in result.edges:
            value = sum(vertex[mask] for vertex in edge.verts) * 0.5
            band = "low" if value < 0.2 else ("mid" if value < 0.8 else "high")
            bands[band].append(edge.calc_length())
        means = {key: sum(values) / len(values) for key, values in bands.items() if values}
        counts = {key: len(values) for key, values in bands.items()}
        collapsed = sum(row["collapsed"] for row in info["iterations"])
        split = sum(row["split"] for row in info["iterations"])
        final = (len(result.verts), len(result.faces))
        print("DENSITY_PROBE", voxel, initial, final, means, counts,
              {"collapsed": collapsed, "split": split}, flush=True)
        assert collapsed > 0, info
        assert counts["high"] > 0, counts
        lower = min(means["low"], means["high"])
        upper = max(means["low"], means["high"])
        assert lower * .9 <= means["mid"] <= upper * 1.1, means
    finally:
        seed.free()
        if result is not None:
            result.free()
        bpy.data.meshes.remove(mesh)


for size in (0.2, 0.35, 0.5):
    run(size)

print("PASS: dense-to-coarse mask transition remains measurable", flush=True)
