"""Non-destructive V0.6 -> BSE V27 brush library migration.
Run in installed experimental Blender, in background with --python-exit-code 1.
Supply either the trusted original addon ZIP (rebuild original balanced recipes)
or a saved blend containing all original managed assets (preserve artist edits).
No per-dab Python, no changes to original files or preferences.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile
import zipfile

# Exact names/recipe IDs confirmed by V0.6 documentation. Other recipes
# are routed conservatively by actual native brush type, not guessed names.
# (native type, action, strength, mobility, plasticity, family)
RECIPES = {
    "BUILD": ("CLAY_STRIPS", "AUTONOMOUS_DEPOSIT", 1.0, .85, .95, "Clay"),
    "SCRAPER": ("CLAY_STRIPS", "AUTONOMOUS_SCRAPE", 1.75, .90, .85, "Clay"),
    "STONE_CHISEL": ("CLAY_STRIPS", "AUTONOMOUS_SCRAPE", 1.35, .65, .45, "Stone"),
    "EDGE_BREAK": ("CLAY_STRIPS", "AUTONOMOUS_SCRAPE", 1.65, .55, .40, "Stone"),
    "ROCK_POLISH": ("CLAY", "AUTONOMOUS_MELT", .82, .45, .50, "Stone"),
    "WAX_KNIFE": ("CLAY_STRIPS", "AUTONOMOUS_SCRAPE", 1.30, .65, .85, "Wax"),
    "WOOD_GOUGE": ("CLAY_STRIPS", "AUTONOMOUS_SCRAPE", 1.15, .65, .60, "Wood"),
    "WOOD_CHISEL": ("CLAY_STRIPS", "AUTONOMOUS_SCRAPE", 1.25, .65, .60, "Wood"),
    "MUD_BUILDUP": ("CLAY_STRIPS", "AUTONOMOUS_DEPOSIT", 1.0, .95, 1.0, "Mud"),
    "MUD_SMEAR": ("CLAY_STRIPS", "AUTONOMOUS_SMEAR", 1.15, .95, 1.0, "Mud"),
    "LUMPY_PUTTY": ("CLAY", "AUTONOMOUS_DEPOSIT", 1.0, .90, .98, "Mud"),
}
ALIASES = {
    "claybuild": "BUILD", "claybulb": "BUILD", "claybuildup": "BUILD",
    "loopscraper": "SCRAPER", "stonechisel": "STONE_CHISEL",
    "edgebreak": "EDGE_BREAK", "rockpolish": "ROCK_POLISH",
    "waxknife": "WAX_KNIFE", "woodgouge": "WOOD_GOUGE",
    "woodchisel": "WOOD_CHISEL", "mudbuildup": "MUD_BUILDUP",
    "mudsmear": "MUD_SMEAR", "lumpyputty": "LUMPY_PUTTY",
    "stonecrack": "STONE_CRACK", "grainrake": "GRAIN_RAKE",
    "splinterstamp": "SPLINTER_STAMP",
}
# Preserve actual native tool on these recipes: autonomous surface solver
# cannot reproduce multiplane fracture, narrow cuts, grain or stamps.
NATIVE_ONLY = {"STONE_CRACK", "GRAIN_RAKE", "SPLINTER_STAMP"}
ASSISTED = {"CLAY", "CLAY_STRIPS", "SMOOTH", "DRAW", "NUDGE", "GRAVITY"}
def recipe_key(identifier, name):
    key = str(identifier).upper()
    if key in RECIPES or key in NATIVE_ONLY:
        return key
    norm = re.sub(r"[^a-z0-9]", "", name.lower())
    # Names in asset variants may contain a prefix/suffix. Never choose
    # ambiguous partial matches.
    matches = {v for k,v in ALIASES.items() if k in norm}
    return next(iter(matches)) if len(matches)==1 else key

def route(identifier, name, tool, stroke_method):
    key = recipe_key(identifier, name)
    if key in NATIVE_ONLY or stroke_method in {"DRAG_DOT", "ANCHORED"}:
        return "NATIVE", None, key
    if key in RECIPES:
        return "MATERIAL", RECIPES[key], key
    if tool in ASSISTED:
        return "ASSISTED", None, key
    return "NATIVE", None, key

def self_test():
    assert route("BUILD","Clay Build","CLAY","SPACE")[0]=="MATERIAL"
    assert route("WAX_KNIFE","Wax Knife","CREASE","SPACE")[1][1]=="AUTONOMOUS_SCRAPE"
    assert route("STONE_CRACK","Stone Crack","DRAW","SPACE")[0]=="NATIVE"
    assert route("FOLD","Skin Fold","CLOTH","SPACE")[0]=="NATIVE"
    assert route("OTHER","Custom","CLAY","SPACE")[0]=="ASSISTED"
    assert route("OTHER","Custom","CLAY","DRAG_DOT")[0]=="NATIVE"
    assert route("UNKNOWN","Wood Gouge","CREASE","SPACE")[2]=="WOOD_GOUGE"
    print("V27 material routing self-test passed")

def copy_preview(src, dst):
    if not src.preview or not all(src.preview.image_size):
        return False
    p = dst.preview_ensure()
    p.image_size = tuple(src.preview.image_size)
    p.image_pixels = list(src.preview.image_pixels)
    if all(src.preview.icon_size):
        p.icon_size = tuple(src.preview.icon_size)
        p.icon_pixels = list(src.preview.icon_pixels)
    return True

def load_zip(path, bpy):
    # Execute only the explicitly supplied trusted original extension.
    # Keep its files available until the asset library has been written.
    tmp = tempfile.TemporaryDirectory(prefix="bse_v06_original_")
    folder = Path(tmp.name).resolve()
    with zipfile.ZipFile(path) as z:
        if z.testzip() is not None or not all((folder/n).resolve().is_relative_to(folder) for n in z.namelist()):
            raise RuntimeError("Invalid or unsafe original ZIP")
        z.extractall(folder)
    candidates = [p for p in folder.rglob("__init__.py") if (p.parent/"blender_manifest.toml").is_file()]
    if len(candidates)!=1:
        raise RuntimeError("Expected exactly one extension root")
    init = candidates[0]
    spec = importlib.util.spec_from_file_location("bse_original_v06",init,
                                               submodule_search_locations=[str(init.parent)])
    module=importlib.util.module_from_spec(spec)
    sys.modules[spec.name]=module
    spec.loader.exec_module(module)
    if len(module.BRUSHES)!=31:
        raise RuntimeError("Expected V0.6 with 31 original recipes")
    result=[module._make_brush(s,"BALANCED") for s in module.BRUSHES]
    if any(str(b.get("se_material_recipe_version"))!="0.6" for b in result):
        raise RuntimeError("Source is not the expected V0.6 recipe version")
    return result,tmp

def load_blend(path,bpy):
    with bpy.data.libraries.load(str(path),link=False) as (src,dst):
        dst.brushes=list(src.brushes)
    return [b for b in dst.brushes if b and b.asset_data and
            b.get("se_material_id") and str(b.get("se_material_recipe_version"))=="0.6"],None

def main():
    parser=argparse.ArgumentParser()
    source=parser.add_mutually_exclusive_group(required=False)
    source.add_argument("--source-blend",type=Path)
    source.add_argument("--addon-zip",type=Path)
    parser.add_argument("--output",type=Path)
    parser.add_argument("--self-test",action="store_true")
    args=parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else sys.argv[1:])
    if args.self_test:
        self_test()
        return
    if not (args.source_blend or args.addon_zip) or not args.output:
        parser.error("Pass --source-blend or --addon-zip, and --output")
    import bpy
    props=bpy.types.Brush.bl_rna.properties
    required=("sculpt_physics_mode","sculpt_physics_clay_behavior","sculpt_physics_assist",
              "sculpt_physics_spread","sculpt_physics_plasticity")
    if any(props.get(k) is None for k in required):
        raise RuntimeError("Install experimental BSE Brush RNA first")
    actions=set(props["sculpt_physics_clay_behavior"].enum_items.keys())
    tools=set(props["sculpt_brush_type"].enum_items.keys())
    if not {v[1] for v in RECIPES.values()}<=actions or not {v[0] for v in RECIPES.values()}<=tools:
        raise RuntimeError("Required physical actions or brush types unavailable")
    output=args.output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise RuntimeError("Output must be a new empty library folder")
    originals,keep_tmp=(load_blend(args.source_blend,bpy) if args.source_blend else load_zip(args.addon_zip,bpy))
    if len({str(b.get("se_material_id")) for b in originals})!=31:
        raise RuntimeError("Source does not contain all 31 distinct V0.6 recipe IDs; no output written")
    assets=[]
    report=[]
    seen=set()
    for original in originals:
        identifier=str(original.get("se_material_id"))
        # A saved project can contain multiple user variants; export all
        # separately, without guessing which is the canonical recipe.
        if original.name in seen:
            raise RuntimeError("Duplicate source brush name")
        seen.add(original.name)
        mode,spec,key=route(identifier,original.name,original.sculpt_brush_type,original.stroke_method)
        brush=original.copy()
        brush.name=original.name+" | "+("Physical V27" if mode=="MATERIAL" else "XPBD V27" if mode=="ASSISTED" else "Native Reference V27")
        if not brush.asset_data:
            brush.asset_mark()
        preview_ok=copy_preview(original,brush)
        if not preview_ok:
            raise RuntimeError("Original preview unavailable: "+original.name)
        brush.sculpt_physics_mode=mode
        if mode=="MATERIAL":
            tool,action,strength,spread,plasticity,family=spec
            brush.sculpt_brush_type=tool
            brush.sculpt_physics_clay_behavior=action
            brush.sculpt_physics_assist=1.0
            brush.sculpt_physics_spread=spread
            brush.sculpt_physics_plasticity=plasticity
            brush.strength=strength
            brush.stroke_method="SPACE"
            brush.spacing=min(8,max(3,int(original.spacing)))
            # Scrape is already subtractive for ADD. SUBTRACT would invert it.
            brush.direction="ADD"
            brush.use_plane_trim=False
            if action=="AUTONOMOUS_DEPOSIT":
                brush.use_accumulate=True
        elif mode=="ASSISTED":
            brush.sculpt_physics_assist=.65
        # Keep original textures, mapping, falloff curves, size, pressure
        # and preview. Changed native tool means shape parity is NOT assured.
        if brush.texture != original.texture or brush.mask_texture != original.mask_texture:
            raise RuntimeError("Unexpected texture replacement: "+original.name)
        brush["bse_v27_source_recipe"]=identifier
        brush["bse_v27_source_name"]=original.name
        brush["bse_v27_route"]=mode
        brush.asset_data.description=(original.asset_data.description or "")+" | V27 "+mode+" experimental adaptation."
        brush.asset_data.tags.new("BSE V27")
        assets.append(brush)
        report.append(dict(source=original.name,recipe=identifier,route=mode,
                           asset=brush.name,original_tool=original.sculpt_brush_type,
                           tool=brush.sculpt_brush_type,action=spec[1] if spec else None,
                           strength=brush.strength,preview=preview_ok,
                           alpha=brush.texture.name if brush.texture else None,
                           limitation="Native tool retained; specialized physical law pending" if mode=="NATIVE" else
                           "New footprint/solver requires artist validation" if mode=="MATERIAL" else
                           "Native target with XPBD; no new constitutive law"))
    output.mkdir(parents=True,exist_ok=True)
    target=output/"bse_material_brushes_v27.blend"
    temp=output/"bse_material_brushes_v27.tmp.blend"
    try:
        bpy.data.libraries.write(str(temp),set(assets),fake_user=True,compress=True)
        with bpy.data.libraries.load(str(temp),link=False) as (src,dst):
            if set(b.name for b in assets)!=set(src.brushes):
                raise RuntimeError("Written brush names differ from expected assets")
        temp.replace(target)
        # Preserve catalog IDs on copied assets; copy available catalog metadata.
        if args.source_blend:
            cats=args.source_blend.resolve().parent/"blender_assets.cats.txt"
            if cats.is_file():
                (output/cats.name).write_bytes(cats.read_bytes())
        (output/"migration_report.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
        print("V27 migration:",len(assets),"assets", {m:sum(r["route"]==m for r in report) for m in ("MATERIAL","ASSISTED","NATIVE")},flush=True)
    finally:
        if temp.exists():temp.unlink()
        if keep_tmp:keep_tmp.cleanup()

if __name__=="__main__":
    main()
