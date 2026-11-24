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
    # Sculpt a rounded clay patch, not a rectangular heightmap or a glyph.
    # Each effect has a readable silhouette and broad, smooth forms.
    rings, sides = 64, 128
    verts = [(0., 0., .22 + preview_height(kind, 0., 0.))]
    for j in range(1, rings+1):
        r = j/rings
        for i in range(sides):
            t = 2*math.pi*i/sides
            x = .94*r*math.cos(t)
            y = .74*r*math.sin(t)
            # Flow pulls the leading edge out of the original silhouette.
            if kind in {"flow","direction","hybrid"}:
                x += .17*r*max(0.,math.cos(t))**4
                y += .10*r*max(0.,math.cos(t))**3
            dome = .22*math.sqrt(max(0.,1-r*r))
            blend = min(1.,max(0.,(1-r)/.16))
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
        obj.rotation_euler.z = math.radians(30)
        camera_data = bpy.data.cameras.new("BSE Thumbnail Camera")
        camera = bpy.data.objects.new("BSE Thumbnail Camera",camera_data)
        scene.collection.objects.link(camera)
        camera.location = (0.,-2.8,4.8)
        camera.rotation_euler = (Vector((0.,0.,.04))-camera.location).to_track_quat("-Z","Y").to_euler()
        camera_data.type = "ORTHO"
        camera_data.ortho_scale = 2.45
        scene.camera = camera
        scene.render.engine = "BLENDER_WORKBENCH"
        shade = scene.display.shading
        shade.light = "MATCAP"
        shade.studio_light = matcaps[0]
        shade.color_type = "SINGLE"
        shade.show_shadows = False
        shade.show_cavity = False
        shade.show_outline = False
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
    args = parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else [])
    # Built-in RNA properties belong to instances; hasattr on bpy.types.Brush
    # does not reliably detect them. Inspect the registered RNA schema.
    required = ("sculpt_physics_mode", "sculpt_physics_clay_behavior",
                "sculpt_physics_assist", "sculpt_physics_spread",
                "sculpt_physics_yield", "sculpt_physics_plasticity")
    props = bpy.types.Brush.bl_rna.properties
    missing = [name for name in required if props.get(name) is None]
    if missing:
        raise RuntimeError(
            f"Experimental Brush RNA unavailable in {bpy.app.binary_path}: "
            + ", ".join(missing))
    print("[BSE Physical Brushes] Brush RNA verified", flush=True)
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
        if mode != "ASSISTED" or not copy_preview(bases[base],brush):
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
