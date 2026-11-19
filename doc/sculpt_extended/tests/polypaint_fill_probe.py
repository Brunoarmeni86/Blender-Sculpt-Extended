import bpy
import json
import os
from pathlib import Path


root = Path(os.environ["POLYPAINT_FILL_PROBE_ROOT"])
root.mkdir(parents=True, exist_ok=True)

bpy.ops.object.mode_set(mode="OBJECT") if bpy.context.object else None
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.ops.mesh.primitive_cube_add()
ob = bpy.context.object
modifier = ob.modifiers.new("Multires", "MULTIRES")
for _ in range(3):
    assert bpy.ops.object.multires_subdivide(modifier=modifier.name, mode="CATMULL_CLARK") == {"FINISHED"}

assert bpy.ops.sculpt.multires_polypaint_attribute(action="ADD", name="FillProbe") == {"FINISHED"}
assert bpy.ops.object.mode_set(mode="SCULPT") == {"FINISHED"}
assert bpy.ops.sculpt.polypaint_blur_probe_v01(action="PAINT") == {"FINISHED"}
brush = bpy.context.tool_settings.sculpt.brush
brush.color = (0.125, 0.375, 0.625)
bpy.context.scene.sculpt_polypaint_fill_color = (0.125, 0.375, 0.625)
area = next(area for area in bpy.context.window.screen.areas if area.type == "VIEW_3D")
region = next(region for region in area.regions if region.type == "WINDOW")
with bpy.context.temp_override(area=area, region=region):
    assert bpy.ops.wm.tool_set_by_id(name="builtin.polypaint_fill") == {"FINISHED"}
if bpy.context.tool_settings.sculpt.brush:
    bpy.context.tool_settings.sculpt.brush.color = (0.0, 0.0, 0.0)
keymap = bpy.context.window_manager.keyconfigs.active.keymaps.get(
    "3D View Tool: Sculpt, Polypaint Fill")
assert keymap is not None
assert any(item.idname == "sculpt.polypaint_fill_primary" for item in keymap.keymap_items)
assert bpy.ops.sculpt.polypaint_fill_primary() == {"FINISHED"}

export = root / "fill.json"
assert bpy.ops.sculpt.polypaint_blur_probe_v01(action="EXPORT", filepath=str(export)) == {"FINISHED"}
data = json.loads(export.read_text())
assert data["present"] and all(data["present"])
assert not data["nonuniform"]
expected = (0.125, 0.375, 0.625, 1.0)
assert data["colors"] and all(
    max(abs(actual - wanted) for actual, wanted in zip(sample, expected)) < 1e-6
    for sample in data["colors"]
)

assert bpy.ops.ed.undo() == {"FINISHED"}
# Do not change mode between Undo and Redo: a mode change would create a new
# branch in Blender's history. The pre-fill checkpoint has no dense master.
modifier = next(item for item in bpy.context.object.modifiers if item.type == "MULTIRES")
assert modifier.polypaint_level == 0
assert bpy.ops.ed.redo() == {"FINISHED"}
if bpy.context.object.mode != "SCULPT":
    assert bpy.ops.object.mode_set(mode="SCULPT") == {"FINISHED"}
redo_export = root / "redo.json"
assert bpy.ops.sculpt.polypaint_blur_probe_v01(action="EXPORT", filepath=str(redo_export)) == {"FINISHED"}
redo_data = json.loads(redo_export.read_text())
assert redo_data["colors_hash"] == data["colors_hash"]

print("[POLYPAINT_FILL_PROBE] PASS fill, uniform master, Undo and Redo", flush=True)
bpy.ops.wm.quit_blender()
