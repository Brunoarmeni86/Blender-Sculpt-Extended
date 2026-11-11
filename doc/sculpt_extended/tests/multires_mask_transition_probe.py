"""Read-only fingerprint matrix for intermittent Multires mask loss."""

import bpy
import os
import traceback
from pathlib import Path


OUT = Path(os.environ.get("SCULPT_MASK_TRANSITION_PROBE", "mask-transition-probe"))
OUT.mkdir(parents=True, exist_ok=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.edit.use_global_undo = True


def view_context():
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    return bpy.context.temp_override(window=window, area=area, region=region)


def audit(label):
    result = bpy.ops.sculpt.multires_base_fingerprint(
        label=label, include_layer_payloads=False, include_surface=False)
    assert result == {"FINISHED"}, (label, result)


def run():
    with view_context():
        if bpy.context.object and bpy.context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj = bpy.context.object
        obj.name = "Multires_Mask_Transition_Probe"
        modifier = obj.modifiers.new("Multires", "MULTIRES")
        bpy.ops.object.mode_set(mode="SCULPT")
        assert bpy.ops.sculpt.multires_layer_add() == {"FINISHED"}
        for _ in range(2):
            assert bpy.ops.object.multires_subdivide(modifier=modifier.name) == {"FINISHED"}
        modifier.levels = modifier.sculpt_levels = modifier.total_levels

        assert bpy.ops.paint.mask_flood_fill(mode="VALUE", value=0.375) == {"FINISHED"}
        audit("MASK_01_FILLED")

        # Layer creation/switch and Strength publication must not own mask data.
        assert bpy.ops.sculpt.multires_layer_add() == {"FINISHED"}
        modifier.active_sculpt_layer_index = 0
        modifier.sculpt_layers[0].strength = 0.5
        modifier.sculpt_layers[0].strength = 1.0
        audit("MASK_02_LAYER_STRENGTH")

        # The most common reported symmetry transition.
        obj.data.use_mirror_x = True
        obj.sculpt_symmetry_reference_valid = True
        obj.sculpt_symmetry_reference_axes = (False, True, False)
        obj.sculpt_symmetry_routing_valid = True
        obj.sculpt_symmetry_combine_local = True
        obj.sculpt_symmetry_combine_local = False
        audit("MASK_03_COMBINE_OFF")

        # Non-RNA cache eviction was previously implicated in Strength doubling.
        obj.data.update()
        bpy.context.view_layer.update()
        bpy.context.view_layer.update()
        audit("MASK_04_CACHE_REBUILD")

        # Group merge is included in this matrix before its dedicated Undo work.
        modifier.active_sculpt_layer_index = 1
        assert bpy.ops.sculpt.multires_layer_group_create() == {"FINISHED"}
        group_id = modifier.sculpt_layers[1].group_id
        modifier.active_sculpt_layer_index = 0
        assert bpy.ops.sculpt.multires_layer_group_add_active(group_id=group_id) == {"FINISHED"}
        audit("MASK_05_GROUP_READY")
        merge_result = bpy.ops.sculpt.multires_layer_group_merge(group_id=group_id)
        assert merge_result == {"FINISHED"}, merge_result
        audit("MASK_06_GROUP_MERGED")

    yield 0.4
    with view_context():
        assert bpy.ops.ed.undo() == {"FINISHED"}
    yield 0.4
    audit("MASK_07_MERGE_UNDO")
    with view_context():
        assert bpy.ops.ed.redo() == {"FINISHED"}
    yield 0.4
    audit("MASK_08_MERGE_REDO")
    (OUT / "PASS.txt").write_text("OPERATIONS PASS; compare MASK hashes in log\n")


steps = run()


def tick():
    try:
        return next(steps)
    except StopIteration:
        bpy.ops.wm.quit_blender()
        return None
    except Exception:
        (OUT / "FAIL.txt").write_text(traceback.format_exc())
        traceback.print_exc()
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(tick, first_interval=0.5, persistent=True)
