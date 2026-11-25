"""V32: create a separate experimental 31-brush library.
No external dependencies, no per-dab Python, no preference or input mutation.
Optional original/V27 .blend: copy matching source brush alphas and previews.
This is a new native-first recipe set, NOT a byte-identical V0.6 recreation.
Run in the experimental Blender with --background --factory-startup.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys

# family, name, native tool, direction (None means native default), strength,
# hardness, spacing, tip ratio, legacy action for Material/Simulation A/B.
RECIPES = [
    ("Clay", "Build", "CLAY_STRIPS", "ADD", .80, .45, 5, .90, "DEPOSIT"),
    ("Clay", "Bulb", "CLAY", "ADD", 1.15, .30, 5, 1., "DEPOSIT"),
    ("Clay", "Loop Scraper", "PLANE", "SUBTRACT", .75, .65, 5, .65, None),
    ("Clay", "Press", "CLAY", "SUBTRACT", .65, .55, 5, 1., "PRESS"),
    ("Clay", "Compact", "CLAY", "SUBTRACT", .45, .45, 5, 1., "COMPACT"),
    ("Clay", "Push", "CLAY_STRIPS", "ADD", .70, .40, 5, .85, "FLOW"),
    ("Flesh", "Volume", "CLAY", "ADD", .45, .15, 5, 1., "DEPOSIT"),
    ("Flesh", "Compression", "CLAY", "SUBTRACT", .35, .15, 6, 1., "COMPRESS"),
    ("Flesh", "Elastic Pull", "ELASTIC_DEFORM", None, .50, .25, 8, 1., None),
    ("Flesh", "Soft Crease", "CREASE", "SUBTRACT", .35, .20, 6, 1., None),
    ("Fabric", "Fold", "CLOTH", None, .50, .35, 8, 1., None),
    ("Fabric", "Tension", "CLOTH", None, .65, .20, 8, 1., None),
    ("Fabric", "Seam", "CREASE", "ADD", .50, .75, 4, 1., None),
    ("Fabric", "Gather", "PINCH", None, .45, .40, 5, 1., None),
    ("Stone", "Chisel", "MULTIPLANE_SCRAPE", None, .85, .85, 5, .55, None),
    ("Stone", "Edge Break", "PLANE", "SUBTRACT", .85, .75, 5, .80, None),
    ("Stone", "Crack", "DRAW_SHARP", "SUBTRACT", .70, .90, 4, 1., None),
    ("Stone", "Rock Polish", "SMOOTH", None, .35, .40, 6, 1., None),
    ("Wax", "Knife", "DRAW_SHARP", "SUBTRACT", .65, .80, 4, .55, None),
    ("Wax", "Build", "CLAY", "ADD", .55, .35, 5, 1., "DEPOSIT"),
    ("Wax", "Melt", "SMOOTH", None, .40, .25, 5, 1., None),
    ("Wood", "Gouge", "CLAY_STRIPS", "SUBTRACT", .75, .65, 4, .45, "SCRAPE"),
    ("Wood", "Chisel", "PLANE", "SUBTRACT", .75, .80, 5, .55, None),
    ("Wood", "Grain Rake", "DRAW_SHARP", "SUBTRACT", .55, .85, 4, .4, None),
    ("Wood", "Splinter Stamp", "DRAW", "ADD", .65, .75, 10, .5, None),
    ("Mud", "Buildup", "CLAY_STRIPS", "ADD", .85, .25, 4, .90, "DEPOSIT"),
    ("Mud", "Smear", "NUDGE", None, .60, .20, 5, 1., None),
    ("Mud", "Lumpy Putty", "CLAY", "ADD", .85, .15, 6, 1., "DEPOSIT"),
    ("Foam", "Buildup", "CLAY", "ADD", .60, .15, 5, 1., "DEPOSIT"),
    ("Foam", "Compression", "CLAY", "SUBTRACT", .45, .25, 6, 1., "COMPACT"),
    ("Foam", "Puff", "INFLATE", None, .40, .15, 5, 1., None),
]
def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())
def self_test():
    assert len(RECIPES)==31 and len({(r[0],r[1]) for r in RECIPES})==31
    assert Counter(r[0] for r in RECIPES)==dict(Clay=6,Flesh=4,Fabric=4,Stone=4,Wax=3,Wood=4,Mud=3,Foam=3)
    assert all((r[2] in {"CLAY","CLAY_STRIPS"}) == (r[-1] is not None) for r in RECIPES)
    print("V32 recipe self-test OK (31 distinct recipes)")
def main():
    p=argparse.ArgumentParser()
    p.add_argument("--output",type=Path)
    p.add_argument("--source-blend",type=Path)
    p.add_argument("--self-test",action="store_true")
    a=p.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else sys.argv[1:])
    self_test()
    if a.self_test:return
    if not a.output:p.error("--output required")
    import bpy
    output=a.output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise RuntimeError("Choose a NEW empty output directory")
    props=bpy.types.Brush.bl_rna.properties
    for k in ("sculpt_physics_mode","sculpt_physics_clay_behavior","sculpt_physics_assist","sculpt_brush_type"):
        if props.get(k) is None:raise RuntimeError("V32 experimental Brush RNA unavailable: "+k)
    sources=[]
    if a.source_blend:
        if not a.source_blend.is_file():raise FileNotFoundError(a.source_blend)
        with bpy.data.libraries.load(str(a.source_blend.resolve()),link=False) as (src,dst):
            dst.brushes=list(src.brushes)
        sources=[b for b in dst.brushes if b]
    def match(family,label):
        wanted={norm(family+label),norm(label+family)}
        if label in {"Loop Scraper","Rock Polish","Edge Break","Grain Rake","Splinter Stamp","Lumpy Putty"}:wanted.add(norm(label))
        found=[]
        for b in sources:
            name=str(b.get("bse_v27_source_name",b.name)).split(" | ")[0]
            if norm(name) in wanted:found.append(b)
        return found[0] if len(found)==1 else None
    def optional(b,key,val):
        if props.get(key) is not None:
            setattr(b,key,val)
    assets=[];report=[]
    for family,label,tool,direction,strength,hardness,spacing,tip,action in RECIPES:
        old=match(family,label)
        name=f"V32 {family} {label}"
        b=old.copy() if old else bpy.data.brushes.new(name,mode="SCULPT")
        b.name=name
        if b.name!=name:raise RuntimeError("Unexpected duplicate brush name: "+name)
        b.use_paint_sculpt=True
        b.sculpt_brush_type=tool
        # Material mode is intentionally limited to native Clay-family.
        b.sculpt_physics_mode="MATERIAL" if action else "NATIVE"
        b.sculpt_physics_assist=1.0 if action else 0.0
        if action:b.sculpt_physics_clay_behavior="AUTONOMOUS_"+action
        b.strength=strength;b.hardness=hardness;b.spacing=spacing
        b.tip_scale_x=tip
        b.stroke_method="SPACE"
        if direction is not None:b.direction=direction
        if tool in {"CLAY","CLAY_STRIPS"}:
            optional(b,"use_accumulate",True)
            optional(b,"use_plane_trim",False)
        if tool=="DRAW" and label=="Splinter Stamp":
            b.stroke_method="ANCHORED"
        if tool=="CLOTH":
            # Distinguish the two native cloth recipes when the enum exists.
            prop=props.get("cloth_deform_type")
            if prop:
                modes=set(prop.enum_items.keys())
                candidate="GRAB" if label=="Fold" else "EXPAND"
                if candidate in modes:b.cloth_deform_type=candidate
        optional(b,"use_pressure_strength",True)
        b.asset_mark()
        b.asset_data.description=("V32 native-first material response; Clay/Clay Strips require V32 MATERIAL mode."
                                  if action else "V32 native tool emulation; specialized physical law not implemented.")
        b.asset_data.tags.new("BSE V32")
        b.asset_data.tags.new(family)
        b["bse_v32_family"]=family;b["bse_v32_route"]="MATERIAL" if action else "NATIVE"
        if old and old.preview and all(old.preview.image_size):
            preview=b.preview_ensure()
            preview.image_size=tuple(old.preview.image_size)
            preview.image_pixels=list(old.preview.image_pixels)
        assets.append(b)
        report.append(dict(name=name,family=family,tool=tool,route=b["bse_v32_route"],
                           action=action,strength=b.strength,source=old.name if old else None,
                           alpha=b.texture.name if b.texture else None,
                           note="New recipe; no procedural fracture/grain/thermal simulation"))
    output.mkdir(parents=True,exist_ok=True)
    target=output/"bse_material_brushes_v32.blend"
    tmp=output/"bse_material_brushes_v32.tmp.blend"
    try:
        bpy.data.libraries.write(str(tmp),set(assets),fake_user=True,compress=True)
        with bpy.data.libraries.load(str(tmp),link=False) as (src,dst):
            if set(src.brushes)!={b.name for b in assets}:raise RuntimeError("Brush library verification failed")
        tmp.replace(target)
        (output/"v32_recipes.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
        print("V32:",len(assets),"brushes written to",target)
    finally:
        if tmp.exists():tmp.unlink()
if __name__=="__main__":main()
