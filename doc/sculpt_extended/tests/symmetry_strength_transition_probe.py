"""Runtime probe for #12: combined symmetry -> local symmetry after a real layer stroke.

This is a diagnostic, not a correctness fixture.  It records the live CCG, persistent
layer payloads and native base independently around the transition that the user reports.
"""

import bpy
import json
import math
import os
import time
from pathlib import Path
import traceback
from bpy_extras import view3d_utils


OUT = Path(os.environ.get("SCULPT_SYMMETRY_TRANSITION_PROBE", "symmetry-transition-probe"))
OUT.mkdir(parents=True, exist_ok=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.edit.use_global_undo = True


def view_context():
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    return bpy.context.temp_override(window=window, area=area, region=region)


def snapshot(label):
    path = OUT / f"{label}.json"
    with view_context():
        result = bpy.ops.sculpt.polypaint_blur_probe_v01(
            action="EXPORT", filepath=str(path))
        assert result == {"FINISHED"}, (label, result)
        result = bpy.ops.sculpt.multires_base_fingerprint(
            label=label, include_layer_payloads=True, include_surface=True)
        assert result == {"FINISHED"}, (label, result)
    data = json.loads(path.read_text())
    print("[SYMMETRY_TRANSITION_PROBE]", label,
          "positions=" + data["positions_hash"],
          "layers=" + data["layer_hash"], flush=True)
    return data


def position_error(left, right):
    assert len(left["positions"]) == len(right["positions"])
    max_component = 0.0
    sum_squared = 0.0
    components = 0
    for a, b in zip(left["positions"], right["positions"]):
        for x, y in zip(a, b):
            delta = abs(x - y)
            max_component = max(max_component, delta)
            sum_squared += delta * delta
            components += 1
    return max_component, math.sqrt(sum_squared / max(1, components))


def activate_draw_brush():
    with view_context():
        result = bpy.ops.brush.asset_activate(
            asset_library_type="ESSENTIALS",
            relative_asset_identifier=(
                "brushes/essentials_brushes-mesh_sculpt.blend/Brush/Draw"))
        assert result == {"FINISHED"}, result
        brush = bpy.context.tool_settings.sculpt.brush
        brush.use_locked_size = "SCENE"
        brush.unprojected_size = 0.24
        brush.size = 72
        brush.strength = 0.35
        brush.use_pressure_size = False
        brush.use_pressure_strength = False
        brush.stroke_method = "SPACE"
        brush.spacing = 12
        brush.jitter = 0
        brush.use_smooth_stroke = False
        brush.auto_smooth_factor = 0
        return brush


def real_stroke():
    with view_context():
        region = bpy.context.region
        rv3d = bpy.context.space_data.region_3d
        samples = []
        # Deliberately off both symmetry planes so the four intended passes do not
        # overlap at the center merely because of the fixture.
        for i, offset in enumerate((-18, -9, 0, 9, 18)):
            mouse = (region.width * 0.5 + 62 + offset, region.height * 0.5 + 37)
            origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, mouse)
            direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, mouse)
            hit, location, *_ = bpy.context.scene.ray_cast(
                bpy.context.evaluated_depsgraph_get(), origin, direction)
            assert hit, ("stroke missed fixture", mouse)
            samples.append(dict(
                name="SymmetryTransitionProbe",
                location=tuple(location), mouse=mouse, mouse_event=mouse,
                pressure=1.0, size=72.0, x_tilt=0.0, y_tilt=0.0,
                time=i * 0.1, is_start=i == 0))
        result = bpy.ops.sculpt.brush_stroke(
            "EXEC_DEFAULT", True, stroke=samples, override_location=True,
            brush_toggle="None")
        assert result == {"FINISHED"}, result


def setup():
    with view_context():
        if bpy.context.object and bpy.context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj = bpy.context.object
        obj.name = "Symmetry_Strength_Transition_Probe"
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.subdivide(number_cuts=5)
        bpy.ops.object.mode_set(mode="OBJECT")
        for vertex in obj.data.vertices:
            direction = vertex.co.normalized()
            vertex.co = direction * (1.0 + 0.025 * math.sin(direction.x * 9.0))
        obj.data.color_attributes.new(name="Color", type="FLOAT_COLOR", domain="CORNER")
        obj.modifiers.new("Multires", "MULTIRES")
        bpy.ops.object.mode_set(mode="SCULPT")
        bpy.ops.view3d.view_axis(type="TOP", align_active=False)
        bpy.context.space_data.region_3d.view_distance = 3.6
        activate_draw_brush()
        assert bpy.ops.sculpt.multires_layer_add() == {"FINISHED"}
        modifier = obj.modifiers["Multires"]
        for _ in range(2):
            assert bpy.ops.object.multires_subdivide(modifier="Multires") == {"FINISHED"}
        modifier.levels = modifier.sculpt_levels = modifier.total_levels
        obj.data.use_mirror_x = True
        obj.data.use_mirror_y = False
        obj.data.use_mirror_z = False
        settings = obj.sculpt_symref_preview
        settings.reference_axes = (False, True, False)
        obj.sculpt_symmetry_reference_location = (0.0, 0.0, 0.0)
        obj.sculpt_symmetry_reference_rotation = (0.0, 0.0, 0.0)
        obj.sculpt_symmetry_reference_valid = True
        obj.sculpt_symmetry_reference_axes = (False, True, False)
        obj.sculpt_symmetry_routing_valid = True
        obj.sculpt_symmetry_combine_local = True
        return obj, settings


def run():
    obj, settings = setup()
    yield 0.5
    before = snapshot("00_before_stroke")
    real_stroke()
    after_stroke = snapshot("01_after_stroke_pending")

    # Assign the native RNA Boolean drawn by the UI checkbox. Its setter must
    # publish pending CCG coordinates before changing future stroke routing.
    with view_context():
        start = time.perf_counter()
        obj.sculpt_symmetry_combine_local = False
        disable_ms = (time.perf_counter() - start) * 1000.0
    immediate = snapshot("02_toggle_immediate")
    bpy.context.view_layer.update()
    yield 0.5
    after_update = snapshot("03_toggle_after_update")

    assert after_stroke["positions_hash"] != before["positions_hash"], \
        "The diagnostic brush stroke did not modify the live surface"
    assert immediate["positions_hash"] == after_stroke["positions_hash"], \
        "Disabling combined symmetry changed the live surface immediately"
    assert after_update["positions_hash"] == after_stroke["positions_hash"], \
        "Dependency-graph evaluation changed the committed surface"
    assert immediate["layer_hash"] != after_stroke["layer_hash"], \
        "Pending coordinates were not published to the Sculpt Layer"
    assert after_update["layer_hash"] == immediate["layer_hash"], \
        "Dependency-graph evaluation changed the committed layer payload"

    with view_context():
        commit_result = bpy.ops.sculpt.multires_layer_commit()
        start = time.perf_counter()
        obj.sculpt_symmetry_combine_local = True
        enable_clean_ms = (time.perf_counter() - start) * 1000.0
        start = time.perf_counter()
        obj.sculpt_symmetry_combine_local = False
        disable_clean_ms = (time.perf_counter() - start) * 1000.0
        start = time.perf_counter()
        obj.sculpt_symmetry_reference_axes = (False, False, True)
        axes_clean_ms = (time.perf_counter() - start) * 1000.0
    yield 0.3
    after_commit = snapshot("04_explicit_commit")

    # A generic Multires setting also schedules geometry evaluation. Pending
    # Sculpt Layer coordinates must be published before that evaluation can
    # reach object_update_from_subsurf_ccg and reshape the native base.
    real_stroke()
    before_modifier_update = snapshot("05_before_modifier_update_pending")
    modifier = obj.modifiers["Multires"]
    start = time.perf_counter()
    modifier.quality = 2 if modifier.quality != 2 else 3
    modifier_update_ms = (time.perf_counter() - start) * 1000.0
    bpy.context.view_layer.update()
    yield 0.3
    after_modifier_update = snapshot("06_after_modifier_update")
    modifier_max_error, modifier_rms_error = position_error(
        before_modifier_update, after_modifier_update)

    assert after_modifier_update["layer_hash"] != before_modifier_update["layer_hash"], \
        "A generic Multires update did not publish pending Sculpt Layer coordinates"

    # The geometry update rebuilds the CCG, so a byte-exact live-surface hash is
    # not a valid equivalence test for the packed layer round-trip. The native
    # base fingerprint printed for snapshots 05/06 must remain identical. A
    # clean strength round-trip, however, stays on the local CCG/PBVH path and
    # must restore the evaluated surface within floating-point tolerance.
    layer = modifier.sculpt_layers[0]
    start = time.perf_counter()
    layer.strength = 0.5
    strength_half_ms = (time.perf_counter() - start) * 1000.0
    strength_half = snapshot("07_strength_half")
    start = time.perf_counter()
    layer.strength = 1.0
    strength_restore_ms = (time.perf_counter() - start) * 1000.0
    strength_restored = snapshot("08_strength_restored")
    strength_restore_max_error, strength_restore_rms_error = position_error(
        after_modifier_update, strength_restored)
    assert strength_half["positions_hash"] != after_modifier_update["positions_hash"], \
        "Changing Sculpt Layer strength did not change the live surface"
    assert strength_restore_max_error < 1.0e-5, \
        "A clean Sculpt Layer strength round-trip accumulated visible surface error"

    summary = {
        "combine_after": bool(obj.sculpt_symmetry_combine_local),
        "reference_strokes_after": bool(obj.sculpt_symmetry_reference_use_for_strokes),
        "commit_result": sorted(commit_result),
        "checkbox_timing_ms": {
            "disable_with_pending_stroke": disable_ms,
            "enable_clean": enable_clean_ms,
            "disable_clean": disable_clean_ms,
            "change_reference_axes_clean": axes_clean_ms,
            "modifier_update_with_pending_stroke": modifier_update_ms,
            "strength_to_half_clean": strength_half_ms,
            "strength_restore_clean": strength_restore_ms,
        },
        "surface_error": {
            "modifier_rebuild_max_component": modifier_max_error,
            "modifier_rebuild_rms": modifier_rms_error,
            "strength_roundtrip_max_component": strength_restore_max_error,
            "strength_roundtrip_rms": strength_restore_rms_error,
        },
        "hashes": {
            label: {
                "positions": data["positions_hash"],
                "layers": data["layer_hash"],
            }
            for label, data in (
                ("before", before),
                ("after_stroke", after_stroke),
                ("toggle_immediate", immediate),
                ("toggle_after_update", after_update),
                ("after_commit", after_commit),
                ("before_modifier_update", before_modifier_update),
                ("after_modifier_update", after_modifier_update),
                ("strength_half", strength_half),
                ("strength_restored", strength_restored),
            )
        },
    }
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2))
    (OUT / "PASS.txt").write_text(
        "PASS: live surface preserved; pending stroke published to Sculpt Layer.\n")
    print("[SYMMETRY_TRANSITION_RESULT]", json.dumps(summary), flush=True)
    with view_context():
        bpy.ops.wm.quit_blender()


steps = run()


def tick():
    try:
        return next(steps)
    except StopIteration:
        return None
    except Exception:
        error = traceback.format_exc()
        (OUT / "ERROR.txt").write_text(error)
        print(error, flush=True)
        os._exit(1)


bpy.app.timers.register(tick, first_interval=0.5, persistent=True)
