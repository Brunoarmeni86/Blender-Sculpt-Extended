# SPDX-License-Identifier: GPL-2.0-or-later
"""Runtime sanity check for the Anchored Projection V2 RNA declarations.
Run with the experimental Blender executable --background --factory-startup
--python tests/python/sculpt_extended_anchored_v2_rna_test.py
"""
import bpy


def test():
    props = bpy.types.Brush.bl_rna.properties
    for name in ("use_anchor_surface_projection", "use_anchor_normal_guided_projection"):
        assert name in props, f"Missing Brush RNA property: {name}"

    brushes = bpy.data.brushes
    brush = brushes.new("Anchor V2 RNA Smoke", mode="SCULPT")
    try:
        brush.stroke_method = "ANCHORED"
        brush.texture_slot.map_mode = "AREA_PLANE"
        brush.use_anchor_surface_projection = True
        brush.use_anchor_normal_guided_projection = True
        assert brush.use_anchor_surface_projection
        assert brush.use_anchor_normal_guided_projection

        brush.use_anchor_normal_guided_projection = False
        assert not brush.use_anchor_normal_guided_projection
    finally:
        brushes.remove(brush)
    print("[OK] Anchored Projection V2 RNA smoke test passed")


if __name__ == "__main__":
    test()
