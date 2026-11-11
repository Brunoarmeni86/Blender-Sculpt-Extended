"""Interactive runtime probe for the native symmetry-reference G/R/S operator."""

import bpy
import json
import os
import sys
import traceback
from pathlib import Path
from mathutils import Matrix, Vector


OUT = Path(os.environ.get("SCULPT_GIZMO_PROBE", "gizmo-reference-probe"))
OUT.mkdir(parents=True, exist_ok=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False


def close_enough(left, right, tolerance=1.0e-5):
    return max(abs(a - b) for a, b in zip(left, right)) <= tolerance


def matrix_close(left, right, tolerance=1.0e-7):
    return max(abs(left[i][j] - right[i][j]) for i in range(4) for j in range(4)) <= tolerance


def view_context():
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    return bpy.context.temp_override(window=window, area=area, region=region)


def run_operator(**properties):
    with view_context():
        return bpy.ops.sculpt.reference_native_transform_preview("EXEC_DEFAULT", **properties)


def run():
    module = sys.modules["sculpt_extended_integrated_tools"]
    with view_context():
        if bpy.context.object and bpy.context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj = bpy.context.object
        obj.name = "Gizmo_Reference_Transform_Probe"
        bpy.ops.object.mode_set(mode="SCULPT")

    module.ensure_native_reference(obj)
    obj.sculpt_symmetry_reference_locked = False
    obj.sculpt_symref_preview.anchored = False
    cursor_before = bpy.context.scene.cursor.matrix.copy()

    target = (1.25, -0.5, 0.75)
    result = run_operator(mode="G", target="REFERENCE", has_result=True,
                          result_location=target, result_rotation=(0.0, 0.0, 0.0))
    assert result == {"FINISHED"}, result
    assert close_enough(obj.sculpt_symmetry_reference_location, target)
    assert obj.sculpt_symmetry_reference_mode == "MOVE"
    assert matrix_close(bpy.context.scene.cursor.matrix, cursor_before)

    rotation = (0.15, -0.2, 0.35)
    result = run_operator(mode="R", target="REFERENCE", has_result=True,
                          result_location=target, result_rotation=rotation)
    assert result == {"FINISHED"}, result
    assert close_enough(obj.sculpt_symmetry_reference_rotation, rotation)
    assert obj.sculpt_symmetry_reference_mode == "ROTATE"
    assert matrix_close(bpy.context.scene.cursor.matrix, cursor_before)

    size_before = obj.sculpt_symmetry_reference_display_size
    result = run_operator(mode="S", target="REFERENCE", has_result=True,
                          result_scale=1.5)
    assert result == {"FINISHED"}, result
    assert abs(obj.sculpt_symmetry_reference_display_size - size_before * 1.5) <= 1.0e-6
    assert obj.sculpt_symmetry_reference_mode == "SIZE"
    assert matrix_close(bpy.context.scene.cursor.matrix, cursor_before)

    settings = obj.sculpt_symref_preview
    settings.anchor_mesh = obj.data
    settings.vertex = 0
    settings.face = 0
    settings.topology = (len(obj.data.vertices), len(obj.data.edges), len(obj.data.polygons))
    settings.face_signature = ",".join(str(vertex) for vertex in obj.data.polygons[0].vertices)
    settings.anchored = True
    anchor = module.anchor_position(obj, bpy.context.evaluated_depsgraph_get())
    assert anchor is not None
    anchored_target = (2.0, 1.0, -0.25)
    result = run_operator(mode="G", target="REFERENCE", has_result=True,
                          result_location=anchored_target, result_rotation=rotation)
    assert result == {"FINISHED"}, result
    frame, valid = module.reference_matrix(obj, bpy.context.evaluated_depsgraph_get())
    assert valid
    assert close_enough(frame.translation, anchored_target)
    assert matrix_close(bpy.context.scene.cursor.matrix, cursor_before)

    location_before_invalid = tuple(obj.sculpt_symmetry_reference_location)
    offset_before_invalid = tuple(obj.sculpt_symmetry_anchor_offset)
    settings.topology = (999, 999, 999)
    result = run_operator(mode="G", target="REFERENCE", has_result=True,
                          result_location=(8.0, 8.0, 8.0), result_rotation=rotation)
    assert result == {"CANCELLED"}, result
    assert tuple(obj.sculpt_symmetry_reference_location) == location_before_invalid
    assert tuple(obj.sculpt_symmetry_anchor_offset) == offset_before_invalid
    assert matrix_close(bpy.context.scene.cursor.matrix, cursor_before)

    return {
        "move": list(target),
        "rotation": list(rotation),
        "display_size": obj.sculpt_symmetry_reference_display_size,
        "anchored_move": list(anchored_target),
        "invalid_anchor_rejected": True,
        "cursor_restored": True,
    }


def main():
    try:
        if "sculpt_extended_integrated_tools" not in sys.modules or not hasattr(
                bpy.context.object or bpy.types.Object, "sculpt_symref_preview"):
            return 0.1
        details = run()
        (OUT / "PASS.json").write_text(json.dumps(details, indent=2), encoding="utf-8")
        print("GIZMO_REFERENCE_TRANSFORM_PROBE PASS", json.dumps(details), flush=True)
    except Exception:
        failure = traceback.format_exc()
        (OUT / "FAIL.txt").write_text(failure, encoding="utf-8")
        print("GIZMO_REFERENCE_TRANSFORM_PROBE FAIL\n" + failure, flush=True)
    finally:
        bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(main, first_interval=0.5)
