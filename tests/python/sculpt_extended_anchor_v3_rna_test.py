# SPDX-License-Identifier: GPL-2.0-or-later
"""Anchor V3 RNA smoke test. Use a newly built Blender 5.2.1 fork."""
import bpy

expected = (
    "use_anchor_surface_projection",
    "use_anchor_normal_guided_projection",
    "use_anchor_geodesic_projection",
)
for name in expected:
    assert name in bpy.types.Brush.bl_rna.properties, f"Missing RNA: {name}"

brush = bpy.data.brushes.new("Anchored V3 RNA test", mode="SCULPT")
try:
    brush.stroke_method = 'ANCHORED'
    brush.texture_slot.map_mode = 'AREA_PLANE'
    for name in expected:
        setattr(brush, name, True)
        assert getattr(brush, name), f"RNA option did not set: {name}"
    brush.use_anchor_geodesic_projection = False
    assert not brush.use_anchor_geodesic_projection
finally:
    bpy.data.brushes.remove(brush)
print("[OK] Anchored V3 RNA test")
