"""Build BSE brush assets using the actual installed experimental Blender.

Run in background, with --python-exit-code 1. Output must be a dedicated
new folder. No preferences or existing Essentials assets are modified.
Previews are procedural clay-style illustrations, not solver test renders.
"""
import argparse
import math
import os
from pathlib import Path
import sys
import bpy

CATALOGS = {
    "XPBD Sculpt": "bb859115-c806-45a9-a523-9558ec3e10a8",
    "Physical Brushes": "8b34ca0e-39e2-44a0-9335-748d3c7d9e2a",
}
# Name, Essentials base, mode, action, preview, assist, spread.
SPECS = (
    ("Clay XPBD", "Clay", "ASSISTED", None, "clay", .65, .55),
    ("Clay Strips XPBD", "Clay Strips", "ASSISTED", None, "strips", .65, .55),
    ("Physical Deposit", "Clay", "MATERIAL", "AUTONOMOUS_DEPOSIT", "deposit", .8, .55),
    ("Physical Flow", "Clay Strips", "MATERIAL", "FLOW", "flow", 1., 1.),
    ("Physical Directional Flow", "Clay Strips", "MATERIAL", "AUTONOMOUS_FLOW", "direction", .8, .55),
    ("Physical Compress", "Clay Strips", "MATERIAL", "AUTONOMOUS_COMPRESS", "compress", .75, .55),
    ("Physical Compact", "Clay", "MATERIAL", "AUTONOMOUS_COMPACT", "compact", .75, .45),
    ("Physical Press", "Clay Strips", "MATERIAL", "AUTONOMOUS_PRESS", "press", .75, .45),
    ("Physical Pressure Flow", "Clay Strips", "MATERIAL", "AUTONOMOUS_PRESSURE_FLOW", "pressure", .75, .55),
    ("Physical Hybrid Flow", "Clay Strips", "MATERIAL", "AUTONOMOUS_HYBRID_FLOW", "hybrid", .75, .55),
)

def preview_height(kind, x, y):
    # A sweeping clay stroke, shaded with a common neutral material.
    v = y - .17 * math.sin(2.8*x) - .12*x
    end = math.exp(-((x+.05)/.73)**8)
    def ridge(c, width):
        return math.exp(-((v-c)/width)**2)
    if kind == "clay":
        return .21*end*ridge(0,.29)
    if kind == "strips":
        return .17*end*sum(ridge(k,.085) for k in (-.20,0,.20))
    if kind == "deposit":
        return .28*end*ridge(0,.25)*(1+.10*math.cos(19*x))
    if kind == "direction":
        return end*(.18*ridge(0,.19)+.10*ridge(.19,.12)-.06*ridge(-.21,.13))
    if kind in {"flow","hybrid"}:
        bump = math.exp(-((x-.43)/.22)**2)
        return end*(.12*ridge(-.12,.25)+(.11+.24*bump)*ridge(.12,.14)
                    -(.07 if kind=="hybrid" else .035)*ridge(-.25,.15))
    if kind == "compress":
        return end*(-.22*ridge(0,.24)+.10*(ridge(-.32,.11)+ridge(.32,.11)))
    if kind == "compact":
        return -.24*end*ridge(0,.28)
    if kind == "press":
        return -.21*end*math.exp(-(v/.27)**8)
    return end*(-.11*ridge(0,.25)+.13*(ridge(-.29,.14)+ridge(.29,.14)))

def make_preview(brush, kind):
    n = 128
    step = 2/n
    pixels = []
    for j in range(n):
        y = (j+.5)*step-1
        for i in range(n):
            x = (i+.5)*step-1
            z = preview_height(kind,x,y)
            dx = (preview_height(kind,x+step,y)-preview_height(kind,x-step,y))/(2*step)
            dy = (preview_height(kind,x,y+step)-preview_height(kind,x,y-step))/(2*step)
            light = (.35*dx-.48*dy+.80)/math.sqrt(1+dx*dx+dy*dy)
            shade = max(.05,min(.95,.22+.56*max(0,light)+.1*z))
            fade = max(0,min(1,(1-max(abs(x),abs(y)))/.17))
            shade = .13+(shade-.13)*fade
            pixels.extend((shade,shade,shade,1.))
    p = brush.preview_ensure()
    p.image_size = (n,n)
    p.image_pixels_float = pixels
    # Explicit low-resolution icon for small shelf display.
    p.icon_size = (32,32)
    icon = []
    for y in range(32):
        for x in range(32):
            k = ((y*4+2)*n+x*4+2)*4
            icon.extend(pixels[k:k+4])
    p.icon_pixels_float = icon

def find_source(explicit):
    if explicit:
        p = Path(explicit).resolve()
    else:
        name = "brushes/essentials_brushes-mesh_sculpt.blend"
        root = bpy.utils.system_resource("DATAFILES", path="assets")
        p = Path(root)/name if root else Path()
        if not p.is_file():
            p = Path(bpy.app.binary_path).parent / (
                str(bpy.app.version[0])+"."+str(bpy.app.version[1])) / "datafiles/assets" / name
    if not p.is_file():
        raise RuntimeError("Essentials sculpt library not found; pass --source with its path")
    return p

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output",required=True)
    parser.add_argument("--source")
    args = parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else [])
    if not hasattr(bpy.types.Brush,"sculpt_physics_mode"):
        raise RuntimeError("Run with Blender Sculpt Extended experimental build")
    output = Path(args.output).expanduser().resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise RuntimeError("Choose a new empty dedicated asset library folder")
    source = find_source(args.source)
    names = sorted({spec[1] for spec in SPECS})
    with bpy.data.libraries.load(str(source),link=False) as (src,dst):
        missing = set(names)-set(src.brushes)
        if missing:
            raise RuntimeError(f"Missing Essentials brushes: {sorted(missing)}")
        dst.brushes = names
    bases = dict(zip(names,dst.brushes))
    assets = set()
    for name, base, mode, action, icon, assist, spread in SPECS:
        brush = bases[base].copy()
        if brush.asset_data:
            brush.asset_clear()
        brush.name = name
        if brush.name != name:
            raise RuntimeError(f"Brush name already exists: {name}")
        brush.sculpt_physics_mode = mode
        brush.sculpt_physics_assist = assist
        brush.sculpt_physics_spread = spread
        if action:
            brush.sculpt_physics_clay_behavior = action
        if action == "FLOW":
            brush.sculpt_physics_yield = 1.
            brush.sculpt_physics_plasticity = 1.
            brush.tip_roundness = .15
            brush.use_accumulate = True
        if mode == "MATERIAL":
            brush.stroke_method = "SPACE"
        brush.asset_mark()
        category = "XPBD Sculpt" if mode=="ASSISTED" else "Physical Brushes"
        brush.asset_data.catalog_id = CATALOGS[category]
        brush.asset_data.description = (
            "Native sculpt target with XPBD assistance" if mode=="ASSISTED" else
            "Experimental gesture-driven physical material action: "+action)
        brush.asset_data.author = "Blender Sculpt Extended"
        brush.asset_data.tags.new("BSE")
        make_preview(brush,icon)
        assets.add(brush)
    output.mkdir(parents=True,exist_ok=True)
    target = output/"bse_physical_brushes.blend"
    temp = output/"bse_physical_brushes.tmp.blend"
    try:
        bpy.data.libraries.write(str(temp),assets,fake_user=True,compress=True)
        # Verify the saved library includes all expected brush names.
        with bpy.data.libraries.load(str(temp),link=False) as (src,dst):
            if not {s[0] for s in SPECS}.issubset(src.brushes):
                raise RuntimeError("Written library is missing brush assets")
        os.replace(temp,target)
        (output/"blender_assets.cats.txt").write_text(
            "VERSION 1\n\n"+"".join(f"{cid}:{name}:{name}\n" for name,cid in CATALOGS.items()),
            encoding="utf-8")
    finally:
        if temp.exists():
            temp.unlink()
    print(f"[BSE Physical Brushes] {len(assets)} assets written: {output}")
    print("Add this folder under Preferences > File Paths > Asset Libraries.")

if __name__=="__main__":
    main()
