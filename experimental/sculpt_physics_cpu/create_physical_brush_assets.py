"""Build BSE brush assets using the actual installed experimental Blender.

Run in background, with --python-exit-code 1. Output must be a dedicated
new folder. No preferences or existing Essentials assets are modified.
Previews use actual Workbench geometry and Blender's brush thumbnail MatCap.
They illustrate intended behavior, not measured solver results. Native XPBD
assets retain the corresponding Essentials preview.
"""
import argparse
import math
import os
from pathlib import Path
import sys
import tempfile
import bpy

CATALOGS = {
    "XPBD Sculpt": "bb859115-c806-45a9-a523-9558ec3e10a8",
    "Physical Brushes": "8b34ca0e-39e2-44a0-9335-748d3c7d9e2a",
    "Physical Brushes/Build": "f9c4b635-56c3-4db2-982c-a71b96d282f1",
    "Physical Brushes/Transport": "a66fa0fb-9bd8-44a4-9911-60ff10ad81e2",
    "Physical Brushes/Compression": "2ca7a46a-20f6-4cf5-a0dc-7adfa89903f2",
    "Physical Brushes/Carving": "ca085ce2-6d67-49da-afeb-614f27af470a",
    "Physical Brushes/Thermal": "383a78b3-64ad-4ed4-8fbe-a395b12b618f",
}
# Name, Essentials base, mode, action, preview, assist, spread, catalog, material.
# Legacy Directional/Pressure/Hybrid Flow remain in RNA for older assets.
# No placeholder assets for unimplemented volumetric fluid or fracture.
SPECS = (
    ("Clay XPBD", "Clay", "ASSISTED", None, "clay", .65, .55, "XPBD Sculpt", "SOFT_CLAY"),
    ("Clay Strips XPBD", "Clay Strips", "ASSISTED", None, "strips", .65, .55, "XPBD Sculpt", "FIRM_CLAY"),
    ("Soft Clay Build", "Clay", "MATERIAL", "AUTONOMOUS_DEPOSIT", "deposit", .82, .62, "Physical Brushes/Build", "SOFT_CLAY"),
    ("Firm Clay Build", "Clay", "MATERIAL", "AUTONOMOUS_DEPOSIT", "deposit", .72, .42, "Physical Brushes/Build", "FIRM_CLAY"),
    ("Mud Build", "Clay", "MATERIAL", "AUTONOMOUS_DEPOSIT", "deposit", .88, .92, "Physical Brushes/Build", "MUD"),
    ("Wax Build", "Clay", "MATERIAL", "AUTONOMOUS_DEPOSIT", "deposit", .70, .35, "Physical Brushes/Build", "WAX"),
    ("Putty Build", "Clay", "MATERIAL", "AUTONOMOUS_DEPOSIT", "deposit", .86, .72, "Physical Brushes/Build", "PUTTY"),
    ("Soft Clay Push", "Clay Strips", "MATERIAL", "FLOW", "flow", 1., .75, "Physical Brushes/Transport", "SOFT_CLAY"),
    ("Mud Smear", "Clay Strips", "MATERIAL", "AUTONOMOUS_SMEAR", "smear", .9, 1., "Physical Brushes/Transport", "MUD"),
    ("Putty Gather", "Clay", "MATERIAL", "AUTONOMOUS_GATHER", "gather", .88, .82, "Physical Brushes/Transport", "PUTTY"),
    ("Firm Clay Compress", "Clay Strips", "MATERIAL", "AUTONOMOUS_COMPRESS", "compress", .78, .42, "Physical Brushes/Compression", "FIRM_CLAY"),
    ("Wax Press", "Clay Strips", "MATERIAL", "AUTONOMOUS_PRESS", "press", .78, .32, "Physical Brushes/Compression", "WAX"),
    ("Flesh Press", "Clay Strips", "MATERIAL", "AUTONOMOUS_PRESS", "press", .68, .55, "Physical Brushes/Compression", "FLESH"),
    ("Foam Compact", "Clay", "MATERIAL", "AUTONOMOUS_COMPACT", "compact", .72, .62, "Physical Brushes/Compression", "FOAM"),
    ("Stone Form", "Clay Strips", "MATERIAL", "AUTONOMOUS_PRESS", "press", .62, .18, "Physical Brushes/Compression", "STONE"),
    ("Wood Form", "Clay Strips", "MATERIAL", "AUTONOMOUS_PRESS", "press", .68, .28, "Physical Brushes/Compression", "WOOD"),
    ("Mud Melt (Surface)", "Clay", "MATERIAL", "AUTONOMOUS_MELT", "melt", 1., 1., "Physical Brushes/Thermal", "MUD"),
)

# Two experimental assets only. Preserve the existing D11 library and presets.
D12_SPECS = (
    ("D12 Clay Buildup", "Clay Strips", "MATERIAL", "AUTONOMOUS_DEPOSIT", "d12_clay", 1., .55, "Physical Brushes/Build", "FIRM_CLAY"),
    ("D12 Flesh Flow", "Clay", "MATERIAL", "AUTONOMOUS_SMEAR", "d12_flesh", 1., .55, "Physical Brushes/Transport", "FLESH"),
)

def preview_height(kind, x, y):
    # Broad exaggerated sculpt forms read at 32-48 px. These depict
    # intended brush behavior, not solver output.
    def g(cx, cy, sx, sy):
        return math.exp(-((x-cx)/sx)**2-((y-cy)/sy)**2)
    def ridge(c, width, v=y):
        return math.exp(-((v-c)/width)**2)
    end = math.exp(-((x+.05)/.82)**8)
    v = y - .20*math.sin(3*x)
    if kind == "d12_clay":
        return .43*end*ridge(-.16,.19,v)+.55*end*ridge(.16,.19,v)
    if kind == "d12_flesh":
        return .46*g(.35,0,.40,.34)-.19*g(-.35,0,.37,.31)+.17*end*ridge(.29,.20,v)
    if kind == "clay":
        return .46*end*ridge(0,.33,v)
    if kind == "strips":
        return .36*end*sum(ridge(k,.11,v) for k in (-.24,0,.24))
    if kind == "deposit":
        # Three large, overlapping additive lumps.
        return .57*g(-.48,0,.35,.41)+.66*g(0,.05,.36,.43)+.58*g(.48,-.02,.36,.38)
    if kind == "gather":
        r = math.hypot(x,y)
        return .68*g(0,0,.30,.30)-.17*math.exp(-((r-.53)/.16)**2)
    if kind == "smear":
        return .30*end*ridge(0,.45,v)+.35*g(-.30,.05,.33,.32)-.10*g(.5,-.35,.34,.18)
    if kind == "melt":
        return .23*g(-.34,.10,.55,.42)+.16*g(.30,-.20,.60,.37)
    if kind == "direction":
        return .54*end*ridge(0,.19,v)-.16*end*ridge(.30,.16,v)
    if kind == "flow":
        return .65*g(.50,0,.37,.40)+.31*end*ridge(0,.27,v)-.12*g(-.5,-.3,.36,.21)
    if kind == "hybrid":
        return .56*end*ridge(0,.20,v)+.52*g(.40,.19,.32,.29)-.23*end*ridge(-.29,.17,v)
    if kind == "compress":
        return end*(-.33*ridge(0,.24)+.37*(ridge(-.35,.14)+ridge(.35,.14)))
    if kind == "compact":
        # Wide flattened center with a single rounded shoulder.
        r = math.hypot(x/.69,y/.48)
        return -.36*math.exp(-r**6)+.15*math.exp(-((r-1.05)/.22)**2)
    if kind == "scrape":
        return -.38*end*ridge(0,.35)+.12*g(.58,.38,.22,.15)
    if kind == "press":
        # Flat rectangular impression, strongly defined corners.
        return -.42*math.exp(-(x/.57)**8-(y/.32)**8)+.16*end*(ridge(-.42,.12)+ridge(.42,.12))
    # Pressure spreads out around a circular depression.
    r = math.hypot(x/.55,y/.43)
    return -.38*math.exp(-(r/.65)**4)+.35*math.exp(-((r-1.)/.26)**2)


def copy_preview(source, target):
    src = source.preview
    if not src or not all(src.image_size):
        return False
    dst = target.preview_ensure()
    dst.image_size = tuple(src.image_size)
    dst.image_pixels = list(src.image_pixels)
    if all(src.icon_size):
        dst.icon_size = tuple(src.icon_size)
        dst.icon_pixels = list(src.icon_pixels)
    return True


def preview_mesh(kind):
    # Distinct silhouettes: not eight nearly identical oval medallions.
    # Keep a smooth, solid clay surface and use shape rather than text.
    rings, sides = 64, 128
    verts = [(0., 0., .15 + preview_height(kind, 0., 0.))]
    for j in range(1, rings+1):
        r = j/rings
        for i in range(sides):
            t = 2*math.pi*i/sides
            c, sn = math.cos(t), math.sin(t)
            x, y = .98*r*c, .72*r*sn
            if kind == "melt":
                x *= 1.13
                y *= .90+.20*math.cos(3*t)
            elif kind == "gather":
                x,y=.83*r*c,.83*r*sn
            elif kind in {"flow","direction","hybrid","smear"}:
                x += .48*r*max(0.,c)**8
                y += .24*r*max(0.,c)**5
                y *= .72+.28*max(0.,c)
                if kind == "hybrid":
                    y += .18*r*math.sin(2*t)
            elif kind == "deposit":
                y *= 1+.12*math.cos(6*t)
                x *= 1+.07*math.cos(6*t)
            elif kind == "pressure":
                x, y = .88*r*c,.83*r*sn
                x *= 1+.13*math.cos(4*t)
                y *= 1+.13*math.cos(4*t)
            elif kind == "press":
                # Rounded rectangle, unlike the circular pressure pad.
                x = .91*r*math.copysign(abs(c)**.55,c)
                y = .64*r*math.copysign(abs(sn)**.55,sn)
            elif kind == "compress":
                y *= 1+.18*abs(c)
            elif kind == "compact":
                x *= .88
                y *= 1.10
            dome = .15*math.sqrt(max(0.,1-r*r))
            blend = min(1.,max(0.,(1-r)/.19))
            blend = blend*blend*(3-2*blend)
            z = dome + blend*preview_height(kind,x,y)
            verts.append((x,y,z))
    faces = [(0,1+i,1+(i+1)%sides) for i in range(sides)]
    for j in range(1,rings):
        a, b = 1+(j-1)*sides, 1+j*sides
        for i in range(sides):
            k = (i+1)%sides
            faces.append((a+i,b+i,b+k,a+k))
    # Rounded underside closes the edge for solid silhouettes.
    last = 1+(rings-1)*sides
    bottom = len(verts)
    verts.append((0.,0.,-.19))
    faces.extend((last+i,bottom,last+(i+1)%sides) for i in range(sides))
    mesh = bpy.data.meshes.new("BSE Thumbnail Clay")
    mesh.from_pydata(verts,[],faces)
    mesh.update()
    for poly in mesh.polygons:
        poly.use_smooth = True
    return mesh


def make_preview(brush, kind):
    # Use the installed official MatCap, rather than approximating its
    # colors. Fail clearly if unavailable or if headless Workbench cannot
    # render; do not silently switch back to artificial graphic previews.
    from mathutils import Vector
    lights = bpy.context.preferences.studio_lights
    matcaps = [l.name for l in lights if l.type == "MATCAP"
               and "brush_thumbnail_preview" in l.name.lower()]
    if not matcaps:
        raise RuntimeError("Official brush_thumbnail_preview MatCap unavailable")
    scene = bpy.data.scenes.new("BSE Thumbnail Scene")
    mesh = obj = camera = camera_data = None
    try:
        mesh = preview_mesh(kind)
        obj = bpy.data.objects.new("BSE Thumbnail Clay",mesh)
        scene.collection.objects.link(obj)
        obj.rotation_euler.z = math.radians(38 if kind in {"flow","direction","hybrid","deposit"} else 24)
        camera_data = bpy.data.cameras.new("BSE Thumbnail Camera")
        camera = bpy.data.objects.new("BSE Thumbnail Camera",camera_data)
        scene.collection.objects.link(camera)
        camera.location = (0.,-2.8,4.8)
        camera.rotation_euler = (Vector((0.,0.,.04))-camera.location).to_track_quat("-Z","Y").to_euler()
        camera_data.type = "ORTHO"
        # Fit the actual projected silhouette: at least one dimension fills
        # 90% of the image, regardless of stroke shape or orientation.
        rot = camera.rotation_euler.to_matrix()
        inv = rot.transposed()
        obj_rot = obj.rotation_euler.to_matrix()
        projected = [inv @ (obj_rot @ v.co) for v in mesh.vertices]
        left, right = min(v.x for v in projected), max(v.x for v in projected)
        bottom, top = min(v.y for v in projected), max(v.y for v in projected)
        camera.location += rot @ Vector(((left+right)*.5,(bottom+top)*.5,0.))
        camera_data.ortho_scale = max(right-left,top-bottom)/.90
        scene.camera = camera
        scene.render.engine = "BLENDER_WORKBENCH"
        shade = scene.display.shading
        shade.light = "MATCAP"
        shade.studio_light = matcaps[0]
        shade.color_type = "SINGLE"
        # Optional presentation flags differ between Blender versions.
        # Keep the official MatCap mandatory; omit unsupported cosmetic flags.
        for flag in ("show_shadows", "show_cavity", "show_object_outline"):
            if shade.bl_rna.properties.get(flag) is not None:
                setattr(shade, flag, False)
        scene.render.resolution_x = 256
        scene.render.resolution_y = 256
        scene.render.resolution_percentage = 100
        scene.render.film_transparent = True
        scene.render.image_settings.file_format = "PNG"
        scene.render.image_settings.color_mode = "RGBA"
        with tempfile.TemporaryDirectory(prefix="bse_brush_preview_") as folder:
            file = Path(folder)/"preview.png"
            scene.render.filepath = str(file)
            result = bpy.ops.render.render(scene=scene.name,write_still=True)
            if "FINISHED" not in result or not file.is_file():
                raise RuntimeError("Workbench preview render failed")
            with bpy.context.temp_override(id=brush):
                result = bpy.ops.ed.lib_id_load_custom_preview(filepath=str(file))
            if "FINISHED" not in result or not brush.preview or not all(brush.preview.image_size):
                raise RuntimeError("Could not attach rendered brush thumbnail")
    finally:
        if camera:
            bpy.data.objects.remove(camera,do_unlink=True)
        if obj:
            bpy.data.objects.remove(obj,do_unlink=True)
        if camera_data:
            bpy.data.cameras.remove(camera_data)
        if mesh:
            bpy.data.meshes.remove(mesh)
        bpy.data.scenes.remove(scene)


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
    parser.add_argument("--dedicated-d12",action="store_true")
    args = parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else [])
    specs=D12_SPECS if args.dedicated_d12 else SPECS
    # Built-in RNA properties belong to instances; hasattr on bpy.types.Brush
    # does not reliably detect them. Inspect the registered RNA schema.
    required = ("sculpt_physics_mode", "sculpt_physics_clay_behavior",
                "sculpt_physics_assist", "sculpt_physics_spread",
                "sculpt_physics_yield", "sculpt_physics_plasticity",
                "sculpt_physics_material")
    props = bpy.types.Brush.bl_rna.properties
    missing = [name for name in required if props.get(name) is None]
    if missing:
        raise RuntimeError(
            f"Experimental Brush RNA unavailable in {bpy.app.binary_path}: "
            + ", ".join(missing))
    actions = set(props["sculpt_physics_clay_behavior"].enum_items.keys())
    missing_actions = sorted({spec[3] for spec in specs if spec[3]}-actions)
    if missing_actions:
        raise RuntimeError("Install V23 build first; missing actions: "+", ".join(missing_actions))
    print("[BSE Physical Brushes] Brush RNA verified", flush=True)
    output = Path(args.output).expanduser().resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise RuntimeError("Choose a new empty dedicated asset library folder")
    source = find_source(args.source)
    # Blender replaces entries of the destination list with loaded IDs
    # on leaving the context. Keep requested string names independently.
    names = tuple(sorted({spec[1] for spec in specs}))
    with bpy.data.libraries.load(str(source),link=False) as (src,dst):
        missing = set(names)-set(src.brushes)
        if missing:
            raise RuntimeError(f"Missing Essentials brushes: {sorted(missing)}")
        dst.brushes = list(names)
    loaded = tuple(dst.brushes)
    if len(loaded) != len(names):
        raise RuntimeError(f"Expected {len(names)} base brushes, received {len(loaded)}")
    bases = dict(zip(names,loaded))
    for name, brush in bases.items():
        if brush is None or not isinstance(brush,bpy.types.Brush):
            raise RuntimeError(f"Failed to load Essentials brush: {name}")
    print("[BSE Physical Brushes] Loaded base brushes: "+", ".join(names), flush=True)
    assets = set()
    for name, base, mode, action, icon, assist, spread, category, material in specs:
        brush = bases[base].copy()
        if brush.asset_data:
            brush.asset_clear()
        brush.name = name
        if brush.name != name:
            raise RuntimeError(f"Brush name already exists: {name}")
        brush.sculpt_physics_mode = mode
        brush.sculpt_physics_assist = assist
        brush.sculpt_physics_spread = spread
        brush.sculpt_physics_material = material
        if action:
            brush.sculpt_physics_clay_behavior = action
        if action == "FLOW":
            brush.sculpt_physics_yield = 1.
            brush.sculpt_physics_plasticity = 1.
            brush.use_accumulate = True
        if mode == "MATERIAL":
            brush.stroke_method = "SPACE"
            # Deposit remains the V23 control. Round material tips are
            # independent of their original Clay/Clay Strips brush type.
            if action != "AUTONOMOUS_DEPOSIT":
                brush.tip_roundness = 1.0
                brush.tip_scale_x = 1.0
                brush.hardness = .15
                brush.use_plane_trim = False
                brush.sculpt_physics_assist = 1.0
                brush.sculpt_physics_spread = .85
                brush.sculpt_physics_plasticity = .95
                brush.strength = 1.0
                brush.spacing = 8
                if action == "AUTONOMOUS_SCRAPE":
                    brush.strength = .85
                elif action == "AUTONOMOUS_SMEAR":
                    brush.strength = 1.4
                if "SMOOTH" in props["curve_distance_falloff_preset"].enum_items.keys():
                    brush.curve_distance_falloff_preset = "SMOOTH"
        if args.dedicated_d12:
            # One physical operation per dab; avoid inherited post-smoothing.
            brush.auto_smooth_factor=0.0
            brush.use_accumulate=True
            brush.use_plane_trim=False
            brush.direction="ADD"
            brush.strength=1.0
            brush.spacing=6
            brush.tip_roundness=.30 if action=="AUTONOMOUS_DEPOSIT" else 1.0
            brush.tip_scale_x=1.0
            brush.hardness=.25 if action=="AUTONOMOUS_DEPOSIT" else .05
            brush.sculpt_physics_assist=1.0
            if "SMOOTH" in props["curve_distance_falloff_preset"].enum_items.keys():
                brush.curve_distance_falloff_preset="SMOOTH"
            brush["bse_engine_generation"]="D12"
        brush.asset_mark()
        brush.asset_data.catalog_id = CATALOGS[category]
        brush.asset_data.description = (
            "Native sculpt target with XPBD assistance" if mode=="ASSISTED" else
            "Experimental gesture-driven physical material action: "+action)
        brush.asset_data.author = "Blender Sculpt Extended"
        brush.asset_data.tags.new("BSE")
        if mode != "ASSISTED" or not copy_preview(bases[base],brush):
            print(f"[BSE Physical Brushes] Rendering {name}", flush=True)
            make_preview(brush,icon)
        print(f"[BSE Physical Brushes] Prepared {name}", flush=True)
        assets.add(brush)
    output.mkdir(parents=True,exist_ok=True)
    target = output/"bse_physical_brushes.blend"
    temp = output/"bse_physical_brushes.tmp.blend"
    try:
        bpy.data.libraries.write(str(temp),assets,fake_user=True,compress=True)
        # Verify the saved library includes all expected brush names.
        with bpy.data.libraries.load(str(temp),link=False) as (src,dst):
            if not {s[0] for s in specs}.issubset(src.brushes):
                raise RuntimeError("Written library is missing brush assets")
        os.replace(temp,target)
        (output/"blender_assets.cats.txt").write_text(
            "VERSION 1\n\n"+"".join(f"{cid}:{name}:{name.rsplit(chr(47),1)[-1]}\n" for name,cid in CATALOGS.items()),
            encoding="utf-8")
    finally:
        if temp.exists():
            temp.unlink()
    print(f"[BSE Physical Brushes] {len(assets)} assets written: {output}")
    print("Add this folder under Preferences > File Paths > Asset Libraries.")

if __name__=="__main__":
    main()
