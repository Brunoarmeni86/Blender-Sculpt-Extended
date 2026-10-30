"""Integrated Sculpt Extended tools and native feature configuration."""
import importlib.util
import os
import sys
from pathlib import Path

_module = None

def register():
    global _module
    for name in (
        'BLENDER_SCULPT_LAYER_DELTA_SMOOTH',
        'BLENDER_SCULPT_COARSE_SMOOTH_REFERENCE_V01',
        'BLENDER_MULTIRES_SHARED_INDEX_V01',
        'BLENDER_SCULPT_DELTA_GRID_READ_V01',
        'BLENDER_SCULPT_DRAW_TRANSPORT_V01',
        'BLENDER_SCULPT_SYMMETRY_REFERENCE_V01',
        'BLENDER_SCULPT_MASK_REMESH_V01',
    ):
        os.environ.setdefault(name, '1')
    spec = importlib.util.spec_from_file_location(
        'sculpt_extended_integrated_tools',
        Path(__file__).with_name('symmetry_reference_preview_transition.py'))
    _module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = _module
    spec.loader.exec_module(_module)
    _module.register()

def unregister():
    global _module
    if _module is not None:
        _module.unregister()
        _module = None
