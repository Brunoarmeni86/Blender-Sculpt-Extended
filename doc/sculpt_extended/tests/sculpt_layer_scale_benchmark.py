"""Scale benchmark for geometry-neutral Multires Sculpt Layer operations.

Runs in the interactive candidate binary because Sculpt operators require a 3D view.
Global Undo is disabled deliberately: Undo correctness has a separate open regression,
while this fixture isolates operator and metadata scaling.
"""

import bpy
import json
import os
from pathlib import Path
import time
import traceback


OUT = Path(os.environ.get("SCULPT_LAYER_SCALE_BENCH", "sculpt-layer-scale-benchmark"))
COUNTS = tuple(int(value) for value in os.environ.get(
    "SCULPT_LAYER_SCALE_COUNTS", "10,100,500").split(","))
OUT.mkdir(parents=True, exist_ok=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.edit.use_global_undo = False


def view_context():
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    return bpy.context.temp_override(window=window, area=area, region=region)


def measured(operation):
    started = time.perf_counter()
    result = operation()
    return result, (time.perf_counter() - started) * 1000.0


def create_fixture(count):
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    modifier = obj.modifiers.new("Multires", "MULTIRES")
    bpy.ops.object.mode_set(mode="SCULPT")

    add_samples = []
    with view_context():
        for index in range(count):
            started = time.perf_counter()
            assert bpy.ops.sculpt.multires_layer_add() == {"FINISHED"}
            add_samples.append((time.perf_counter() - started) * 1000.0)
            if index == 0:
                assert bpy.ops.object.multires_subdivide(modifier=modifier.name) == {"FINISHED"}
    assert len(modifier.sculpt_layers) == count
    return obj, modifier, add_samples


def benchmark_count(count):
    _obj, modifier, add_samples = create_fixture(count)
    middle = count // 2
    metrics = {
        "layers": count,
        "add_total_ms": sum(add_samples),
        "add_first_ms": add_samples[0],
        "add_last_ms": add_samples[-1],
        "add_last_10_mean_ms": sum(add_samples[-10:]) / min(10, len(add_samples)),
    }

    _, metrics["select_middle_ms"] = measured(
        lambda: setattr(modifier, "active_sculpt_layer_index", middle))
    _, metrics["rename_layer_ms"] = measured(
        lambda: setattr(modifier.sculpt_layers[middle], "name", f"Layer {count} midpoint"))

    with view_context():
        result, metrics["move_layer_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_move(direction="UP"))
        assert result == {"FINISHED"}

        result, metrics["group_create_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_group_create())
        assert result == {"FINISHED"}
        group_id = modifier.sculpt_layers[modifier.active_sculpt_layer_index].group_id
        assert group_id > 0

        result, metrics["group_rename_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_group_rename(
                group_id=group_id, name=f"Benchmark {count}"))
        assert result == {"FINISHED"}

        result, metrics["group_collapse_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_group_collapse(group_id=group_id))
        assert result == {"FINISHED"}
        result, metrics["group_expand_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_group_collapse(group_id=group_id))
        assert result == {"FINISHED"}

        result, metrics["group_move_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_group_move(
                group_id=group_id, direction="UP"))
        assert result == {"FINISHED"}

        target = min(count - 1, modifier.active_sculpt_layer_index + 2)
        modifier.active_sculpt_layer_index = target
        result, metrics["group_add_layer_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_group_add_active(group_id=group_id))
        assert result == {"FINISHED"}

        grouped_index = next(
            index for index, layer in enumerate(modifier.sculpt_layers)
            if layer.group_id == group_id)
        result, metrics["group_remove_layer_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_group_remove(index=grouped_index))
        assert result == {"FINISHED"}

        result, metrics["group_dissolve_ms"] = measured(
            lambda: bpy.ops.sculpt.multires_layer_group_dissolve(group_id=group_id))
        assert result == {"FINISHED"}

    assert len(modifier.sculpt_layers) == count
    assert all(layer.group_id == 0 for layer in modifier.sculpt_layers)
    return metrics


def run():
    results = []
    for count in COUNTS:
        result = benchmark_count(count)
        results.append(result)
        print("[SCULPT_LAYER_SCALE]", json.dumps(result), flush=True)
        yield 0.1
    (OUT / "summary.json").write_text(json.dumps(results, indent=2))
    (OUT / "PASS.txt").write_text("PASS\n")


steps = run()


def tick():
    try:
        return next(steps)
    except StopIteration:
        bpy.ops.wm.quit_blender()
        return None
    except Exception:
        error = traceback.format_exc()
        (OUT / "FAIL.txt").write_text(error)
        print(error, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(tick, first_interval=0.5, persistent=True)
