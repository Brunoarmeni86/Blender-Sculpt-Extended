"""Session-only UI registration. No file, scene, Undo or geometry changes."""
import importlib.util
from pathlib import Path
import sys
import bpy

if not hasattr(bpy.types, "VIEW3D_PT_multires_polypaint_attributes"):
    name = "multires_sculpt_sidebar_session"
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name("addon") / "__init__.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    module.register()
print("[MULTIRES_SIDEBAR] pronto: N > Multires; nessuna modifica alla scena", flush=True)
