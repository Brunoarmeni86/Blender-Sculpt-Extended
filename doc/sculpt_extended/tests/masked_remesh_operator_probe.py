"""Interactive operator/Undo probe for #14 mask-protected remeshing."""

import bpy
import hashlib
import json
import os
import struct
import traceback
from pathlib import Path


OUT = Path(os.environ.get("SCULPT_MASK_REMESH_PROBE", "masked-remesh-operator-probe"))
OUT.mkdir(parents=True, exist_ok=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.edit.use_global_undo = True
bpy.context.preferences.view.show_splash = False


def view_context():
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    return bpy.context.temp_override(window=window, area=area, region=region)


def fingerprint(mesh):
    digest = hashlib.sha256()
    digest.update(struct.pack("<III", len(mesh.vertices), len(mesh.edges), len(mesh.polygons)))
    for vertex in mesh.vertices:
        digest.update(struct.pack("<3d", *vertex.co))
    for polygon in mesh.polygons:
        digest.update(struct.pack("<I", len(polygon.vertices)))
        digest.update(struct.pack("<" + "I" * len(polygon.vertices), *polygon.vertices))
    for name in (".sculpt_mask", "probe_weight"):
        attribute = mesh.attributes.get(name)
        assert attribute is not None, name
        digest.update(name.encode())
        for item in attribute.data:
            digest.update(struct.pack("<d", float(item.value)))
    return digest.hexdigest()


def component_fingerprints(mesh):
    parts = {}
    for name, values in {
            "positions": (tuple(vertex.co) for vertex in mesh.vertices),
            "polygons": (tuple(polygon.vertices) for polygon in mesh.polygons),
            ".sculpt_mask": (item.value for item in mesh.attributes[".sculpt_mask"].data),
            "probe_weight": (item.value for item in mesh.attributes["probe_weight"].data),
    }.items():
        parts[name] = hashlib.sha256(repr(tuple(values)).encode()).hexdigest()
    return parts


def run():
    protected = os.environ.get("SCULPT_MASK_REMESH_STANDARD_CONTROL") != "1"
    with view_context():
        if os.environ.get("SCULPT_MASK_REMESH_USE_CURRENT") == "1":
            obj = bpy.context.object
            assert obj is not None and obj.type == "MESH"
            mesh = obj.data
            assert mesh.attributes.get(".sculpt_mask") is not None
            assert mesh.attributes.get("probe_weight") is not None
        else:
            if bpy.context.object and bpy.context.object.mode != "OBJECT":
                bpy.ops.object.mode_set(mode="OBJECT")
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
        mesh.sculpt_remesh_protect_mask_preview = protected
        mesh.sculpt_remesh_mask_coarsening = 2.5
        mesh.sculpt_remesh_mask_falloff = 1.0
        # Timer-driven sessions need an explicit editor baseline before the
        # native Sculpt and topology Undo steps can be paired reliably. Keep
        # this as an Object-mode Memfile so custom data layers are included.
        if os.environ.get("SCULPT_MASK_REMESH_USE_CURRENT") != "1":
            assert bpy.ops.ed.undo_push(message="Protected remesh probe baseline") == {"FINISHED"}
    # Let the event loop commit the baseline before entering Sculpt and
    # creating the topology-changing step.
    yield .3
    with view_context():
        bpy.ops.object.mode_set(mode="SCULPT")
        # Exercise the common route: protected remesh follows a native Sculpt
        # mask operation, which must be paired with a Memfile before topology changes.
        if os.environ.get("SCULPT_MASK_REMESH_SKIP_MASK_FILTER") != "1":
            assert bpy.ops.sculpt.mask_filter(
                filter_type="SMOOTH", iterations=1, auto_iteration_count=False) == {"FINISHED"}
        before = fingerprint(mesh)
        before_parts = component_fingerprints(mesh)
        before_counts = (len(mesh.vertices), len(mesh.polygons))
        if os.environ.get("SCULPT_MASK_REMESH_DIRECT") == "1":
            result = bpy.ops.object.voxel_remesh_protected_preview("EXEC_DEFAULT")
        else:
            result = bpy.ops.object.voxel_remesh("EXEC_DEFAULT")
        assert result == {"FINISHED"}, result
        after = fingerprint(mesh)
        after_counts = (len(mesh.vertices), len(mesh.polygons))
        assert after != before and after_counts != before_counts, (before_counts, after_counts)
        assert mesh.attributes.get(".sculpt_mask") is not None
        assert mesh.attributes.get("probe_weight") is not None

    yield .3
    if os.environ.get("SCULPT_MASK_REMESH_SKIP_UNDO") == "1":
        return {
            "before_counts": before_counts,
            "after_counts": after_counts,
            "attributes_preserved": True,
            "undo_exact": None,
            "redo_exact": None,
        }
    with view_context():
        assert bpy.ops.ed.undo.poll(), (bpy.context.mode, bpy.context.area.type)
        undo = bpy.ops.ed.undo()
    assert undo == {"FINISHED"}, undo
    yield .3
    obj = bpy.context.object
    mesh = obj.data
    if mesh.attributes.get(".sculpt_mask") is None and os.environ.get(
            "SCULPT_MASK_REMESH_DIAG_UNDO") == "1":
        states = []
        for index in range(4):
            states.append({
                "index": index,
                "mode": bpy.context.mode,
                "counts": (len(mesh.vertices), len(mesh.polygons)),
                "attributes": sorted(attribute.name for attribute in mesh.attributes),
            })
            with view_context():
                if not bpy.ops.ed.undo.poll():
                    break
                bpy.ops.ed.undo()
            yield .3
            obj = bpy.context.object
            mesh = obj.data
        raise AssertionError(("Undo custom-data diagnostic", states))
    undo_hash = fingerprint(mesh)
    assert undo_hash == before, (
        before,
        undo_hash,
        before_counts,
        (len(mesh.vertices), len(mesh.polygons)),
        before_parts,
        component_fingerprints(mesh),
    )

    with view_context():
        redo = bpy.ops.ed.redo()
    assert redo == {"FINISHED"}, redo
    yield .3
    obj = bpy.context.object
    mesh = obj.data
    redo_hash = fingerprint(mesh)
    assert redo_hash == after, (after, redo_hash)
    return {
        "before_counts": before_counts,
        "after_counts": after_counts,
        "attributes_preserved": True,
        "undo_exact": True,
        "redo_exact": True,
    }


steps = run()


def main():
    try:
        return next(steps)
    except StopIteration as completed:
        details = completed.value
        (OUT / "PASS.json").write_text(json.dumps(details, indent=2), encoding="utf-8")
        print("MASKED_REMESH_OPERATOR_PROBE PASS", json.dumps(details), flush=True)
    except Exception:
        failure = traceback.format_exc()
        (OUT / "FAIL.txt").write_text(failure, encoding="utf-8")
        print("MASKED_REMESH_OPERATOR_PROBE FAIL\n" + failure, flush=True)
    finally:
        if (OUT / "PASS.json").exists() or (OUT / "FAIL.txt").exists():
            bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(main, first_interval=.5)
