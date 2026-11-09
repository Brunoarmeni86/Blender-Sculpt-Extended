"""Runtime regression for non-destructive Multires Sculpt Layer groups."""

import bpy
import json
import os
from pathlib import Path
import time
import traceback


OUT = Path(os.environ.get("SCULPT_LAYER_GROUP_PROBE", "sculpt-layer-group-probe"))
OUT.mkdir(parents=True, exist_ok=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.edit.use_global_undo = True


def view_context():
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    return bpy.context.temp_override(window=window, area=area, region=region)


def rows(modifier):
    return [
        {
            "name": layer.name,
            "strength": layer.strength,
            "visible": layer.visible,
            "group_id": layer.group_id,
            "group_name": layer.group_name,
        }
        for layer in modifier.sculpt_layers
    ]


def active_multires():
    obj = bpy.context.object
    modifier = next((item for item in obj.modifiers if item.type == "MULTIRES"), None) if obj else None
    assert modifier is not None, {
        "active_object": obj.name if obj else None,
        "mode": bpy.context.mode,
        "modifiers": [(item.name, item.type) for item in obj.modifiers] if obj else [],
    }
    return modifier


def run():
    timings = {}
    bpy.ops.object.mode_set(mode="OBJECT") if bpy.context.object else None
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    modifier = obj.modifiers.new("Multires", "MULTIRES")
    bpy.ops.object.mode_set(mode="SCULPT")

    with view_context():
        assert bpy.ops.sculpt.multires_layer_add() == {"FINISHED"}
        bpy.ops.object.multires_subdivide(modifier=modifier.name)
        for _ in range(3):
            assert bpy.ops.sculpt.multires_layer_add() == {"FINISHED"}

        for index, (name, strength) in enumerate(
                zip(("A", "B", "C", "D"), (0.25, 0.5, 0.75, 1.0))):
            modifier.sculpt_layers[index].name = name
            modifier.sculpt_layers[index].strength = strength
        modifier.sculpt_layers[2].visible = False
        # Timer-driven test sessions do not start with a previous undo state.
        assert bpy.ops.ed.undo_push(message="Group probe baseline") == {"FINISHED"}

        # Create a folder from D, then move C into it.
        modifier.active_sculpt_layer_index = 3
        started = time.perf_counter()
        assert bpy.ops.sculpt.multires_layer_group_create() == {"FINISHED"}
        timings["create_ms"] = (time.perf_counter() - started) * 1000.0
        group_id = modifier.sculpt_layers[3].group_id
        assert group_id > 0
        modifier.active_sculpt_layer_index = 2
        assert bpy.ops.sculpt.multires_layer_group_add_active(group_id=group_id) == {"FINISHED"}
        assert [r["name"] for r in rows(modifier)] == ["A", "B", "D", "C"]

        started = time.perf_counter()
        assert bpy.ops.sculpt.multires_layer_group_rename(
            group_id=group_id, name="Primary Detail") == {"FINISHED"}
        timings["rename_ms"] = (time.perf_counter() - started) * 1000.0
        assert {r["group_name"] for r in rows(modifier) if r["group_id"] == group_id} == {
            "Primary Detail"}

        # The complete contiguous block moves across an adjacent ungrouped row.
        started = time.perf_counter()
        assert bpy.ops.sculpt.multires_layer_group_move(
            group_id=group_id, direction="UP") == {"FINISHED"}
        timings["move_group_ms"] = (time.perf_counter() - started) * 1000.0
        assert [r["name"] for r in rows(modifier)] == ["A", "D", "C", "B"]

        # Direct per-row removal keeps D and its payload, then adding it back
        # verifies both directions of the collection-like membership action.
        assert bpy.ops.sculpt.multires_layer_group_remove(index=1) == {"FINISHED"}
        assert [r["name"] for r in rows(modifier)] == ["A", "C", "D", "B"]
        assert modifier.sculpt_layers[2].group_id == 0
        assert bpy.ops.sculpt.multires_layer_group_add_active(group_id=group_id) == {"FINISHED"}
        assert [r["name"] for r in rows(modifier)] == ["A", "C", "D", "B"]

        before_dissolve = {r["name"]: (r["strength"], r["visible"])
                           for r in rows(modifier)}
        assert bpy.ops.sculpt.multires_layer_group_visibility(group_id=group_id) == {"FINISHED"}
        started = time.perf_counter()
        assert bpy.ops.sculpt.multires_layer_group_dissolve(group_id=group_id) == {"FINISHED"}
        timings["dissolve_hidden_ms"] = (time.perf_counter() - started) * 1000.0

    yield 0.3
    final = rows(modifier)
    assert len(final) == 4
    assert all(r["group_id"] == 0 and not r["group_name"] for r in final)
    assert {r["name"]: (r["strength"], r["visible"]) for r in final} == before_dissolve

    after_undo = None
    after_redo = None
    if os.environ.get("SCULPT_LAYER_GROUP_SKIP_UNDO") != "1":
        with view_context():
            assert bpy.ops.ed.undo() == {"FINISHED"}
        yield 0.3
        modifier = active_multires()
        after_undo = rows(modifier)
        assert len(after_undo) == 4
        assert sum(r["group_id"] == group_id for r in after_undo) == 2
        assert {r["group_name"] for r in after_undo if r["group_id"] == group_id} == {
            "Primary Detail"}

        with view_context():
            assert bpy.ops.ed.redo() == {"FINISHED"}
        yield 0.3
        modifier = active_multires()
        after_redo = rows(modifier)
        assert after_redo == final

    # The alternative destructive action removes the group and its two member
    # rows. EXEC_DEFAULT intentionally bypasses only the interactive test dialog.
    with view_context():
        modifier.active_sculpt_layer_index = 1
        assert bpy.ops.sculpt.multires_layer_group_create() == {"FINISHED"}
        delete_group_id = modifier.sculpt_layers[1].group_id
        modifier.active_sculpt_layer_index = 2
        assert bpy.ops.sculpt.multires_layer_group_add_active(
            group_id=delete_group_id) == {"FINISHED"}
        assert bpy.ops.sculpt.multires_layer_group_delete(
            "EXEC_DEFAULT", group_id=delete_group_id) == {"FINISHED"}
    yield 0.3
    after_delete_contents = rows(active_multires())
    assert [row["name"] for row in after_delete_contents] == ["A", "B"]
    (OUT / "summary.json").write_text(json.dumps({
        "after_dissolve": final,
        "after_undo": after_undo,
        "after_redo": after_redo,
        "after_delete_contents": after_delete_contents,
        "timings_ms": timings,
    }, indent=2))
    (OUT / "PASS.txt").write_text("PASS\n")


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
