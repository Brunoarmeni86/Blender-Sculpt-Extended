# SPDX-License-Identifier: GPL-2.0-or-later
"""D15: publish one native, ClayBuildup-inspired Clay Strips asset.
Does not modify the source Essentials library or user preferences."""
import argparse
import os
from pathlib import Path
import sys
import bpy

NAME = "BSE Clay Strips"
CATALOG = "3b22bc48-3be5-4c3f-a2ac-44ce4b5cda9d"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source", type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else [])
    out = args.output.resolve()
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise RuntimeError("Choose a new empty asset directory; old libraries are not overwritten")
    source = args.source
    if source is None:
        root = bpy.utils.system_resource("DATAFILES", path="assets")
        name = "brushes/essentials_brushes-mesh_sculpt.blend"
        source = Path(root)/name if root else Path()
        if not source.is_file():
            version = f"{bpy.app.version[0]}.{bpy.app.version[1]}"
            source = Path(bpy.app.binary_path).parent/version/"datafiles/assets"/name
    if not source.is_file():
        raise RuntimeError("Essentials library not found; supply --source")
    with bpy.data.libraries.load(str(source), link=False) as (src, dst):
        if "Clay Strips" not in src.brushes:
            raise RuntimeError("Essentials Clay Strips not found")
        dst.brushes = ["Clay Strips"]
    brush = dst.brushes[0]
    if brush is None:
        raise RuntimeError("Cannot load Clay Strips")
    brush.asset_clear()
    brush.name = NAME
    if brush.name != NAME:
        raise RuntimeError("Duplicate brush name")
    # Native target, no XPBD, no autonomous material law. Preserve Blender's
    # native trim/contact and direction. Artist calibration is still pending.
    if hasattr(brush, "sculpt_physics_mode"):
        brush.sculpt_physics_mode = "NATIVE"
    brush.stroke_method = "SPACE"
    brush.use_accumulate = True
    brush.strength = .75
    brush.spacing = 6
    brush.tip_roundness = .20
    brush.tip_scale_x = 1.0
    brush.hardness = .25
    brush.auto_smooth_factor = 0.0
    brush.curve_distance_falloff_preset = "SMOOTH"
    brush.asset_mark()
    brush.asset_data.catalog_id = CATALOG
    brush.asset_data.description = "Native Clay Strips tuned for a ClayBuildup-inspired profile; initial calibration."
    brush.asset_data.author = "Blender Sculpt Extended"
    brush.asset_data.tags.new("BSE")
    brush["bse_engine_generation"] = "D15_NATIVE"
    out.mkdir(parents=True, exist_ok=True)
    target = out/"bse_clay_strips.blend"
    temp = out/"bse_clay_strips.tmp.blend"
    try:
        bpy.data.libraries.write(str(temp), {brush}, fake_user=True, compress=True)
        with bpy.data.libraries.load(str(temp), link=False) as (src, dst):
            if set(src.brushes) != {NAME}:
                raise RuntimeError("Expected precisely one brush in the D15 library")
        os.replace(temp, target)
        (out/"blender_assets.cats.txt").write_text(
            "VERSION 1\n\n"+CATALOG+":BSE Sculpt:BSE Sculpt\n", encoding="utf-8")
    finally:
        if temp.exists():
            temp.unlink()
    print("[D15] one native Clay Strips asset:", target)

if __name__ == "__main__":
    main()
