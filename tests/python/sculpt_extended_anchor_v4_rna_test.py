# SPDX-License-Identifier: GPL-2.0-or-later
"""Anchor V4 RNA sanity check, test-only: blender.exe --background --factory-startup --python SCRIPT."""
import bpy

expected = ("use_anchor_surface_projection", "use_anchor_normal_guided_projection",
            "use_anchor_geodesic_projection", "use_anchor_surface_transport")
for name in expected:
    assert name in bpy.types.Brush.bl_rna.properties, f"Missing Brush RNA: {name}"

b = bpy.data.brushes.new("V4 Tangent Transport Probe", mode="SCULPT")
try:
    b.stroke_method = "ANCHORED"
    b.texture_slot.map_mode = "AREA_PLANE"
    for name in expected:
        setattr(b, name, True)
        assert getattr(b, name), f"Can't enable {name}"
    b.use_anchor_surface_transport = False
    assert not b.use_anchor_surface_transport
finally:
    bpy.data.brushes.remove(b)
print("[OK] Anchor Surface Tangent Transport V4 RNA")
