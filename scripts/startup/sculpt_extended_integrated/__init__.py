"""Integrated Sculpt Extended tools and native feature configuration."""
import importlib.util
import os
import sys
from pathlib import Path

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty

_module = None


def _xpbd_sync_environment(self, _context):
    os.environ['BLENDER_SCULPT_XPBD_GRAB_V12'] = (
        '1' if self.sculpt_xpbd_grab_solver == 'XPBD' else '0')
    os.environ['BLENDER_SCULPT_XPBD_BACKEND'] = self.sculpt_xpbd_backend.lower()
    os.environ['BLENDER_SCULPT_XPBD_GRAB_ASSIST'] = str(self.sculpt_xpbd_physical_assist)
    os.environ['BLENDER_SCULPT_XPBD_MATERIAL_COMPLIANCE'] = str(
        self.sculpt_xpbd_material_compliance)
    os.environ['BLENDER_SCULPT_XPBD_DAMPING'] = str(self.sculpt_xpbd_velocity_damping)
    os.environ['BLENDER_SCULPT_PHYSICAL_CLAY_V01'] = (
        '1' if self.sculpt_physical_clay_v01 else '0')
    os.environ['BLENDER_SCULPT_PHYSICAL_CLAY_COMPRESSION'] = str(
        self.sculpt_physical_clay_compression)
    os.environ['BLENDER_SCULPT_PHYSICAL_CLAY_PLASTICITY'] = str(
        self.sculpt_physical_clay_plasticity)
    os.environ['BLENDER_SCULPT_PHYSICAL_CLAY_DRAG'] = str(
        self.sculpt_physical_clay_drag)
    os.environ['BLENDER_SCULPT_PHYSICAL_CLAY_VOLUME'] = str(
        self.sculpt_physical_clay_volume)
    os.environ['BLENDER_SCULPT_PHYSICAL_CLAY_RECOVERY'] = str(
        self.sculpt_physical_clay_recovery)
    os.environ['BLENDER_SCULPT_PHYSICAL_CLAY_MATERIAL'] = self.sculpt_physical_clay_material.lower()
    os.environ['BLENDER_SCULPT_GPU_SMOOTH_A1'] = (
        '1' if self.sculpt_gpu_smooth_a1 else '0')
    os.environ['BLENDER_SCULPT_GPU_SMOOTH_MODE'] = self.sculpt_gpu_smooth_mode.lower()
    os.environ['BLENDER_SCULPT_PHYSICAL_BALLOON'] = (
        '1' if self.sculpt_physical_balloon else '0')
    os.environ['BLENDER_SCULPT_BALLOON_PRESSURE'] = str(self.sculpt_balloon_pressure)
    os.environ['BLENDER_SCULPT_BALLOON_TENSION'] = str(self.sculpt_balloon_tension)
    os.environ['BLENDER_SCULPT_LESION_PREVIEW'] = (
        '1' if self.sculpt_lesion_preview else '0')
    os.environ['BLENDER_SCULPT_LESION_OPENING'] = str(self.sculpt_lesion_opening)
    os.environ['BLENDER_SCULPT_ROLL_ALPHA'] = (
        '1' if self.sculpt_roll_alpha else '0')
    os.environ['BLENDER_SCULPT_ROLL_REPEAT'] = str(self.sculpt_roll_repeat)


def _register_xpbd_runtime_settings():
    wm = bpy.types.WindowManager
    wm.sculpt_xpbd_grab_solver = EnumProperty(
        name="Grab Solver",
        items=(
            ('STANDARD', "Standard", "Use Blender's standard Grab deformation"),
            ('XPBD', "XPBD", "Use the experimental quasistatic XPBD solver"),
        ),
        default='STANDARD',
        update=_xpbd_sync_environment)
    wm.sculpt_xpbd_backend = EnumProperty(
        name="Compute",
        items=(
            ('AUTO', "Auto", "Use the validated CPU reference backend"),
            ('CPU', "CPU", "Use the reference CPU solver"),
            ('CUDA', "CUDA (Experimental)", "Use the native Warp Jacobi CUDA solver"),
        ),
        default='AUTO',
        update=_xpbd_sync_environment)
    wm.sculpt_xpbd_physical_assist = FloatProperty(
        name="Physical Assist", min=0.0, max=1.0, default=0.15,
        description="Blend precise Grab targets toward the XPBD material solution",
        update=_xpbd_sync_environment)
    wm.sculpt_xpbd_material_compliance = FloatProperty(
        name="Compliance", min=0.0, max=1.0, default=0.001, precision=4,
        description="Material softness; zero is maximally stiff",
        update=_xpbd_sync_environment)
    wm.sculpt_xpbd_velocity_damping = FloatProperty(
        name="Damping", min=0.0, max=1000.0, default=60.0,
        description="Velocity damping per second",
        update=_xpbd_sync_environment)
    wm.sculpt_physical_clay_v01 = BoolProperty(
        name="Physical Clay",
        default=False,
        description="Use the experimental elastoplastic Clay Strips solver",
        update=_xpbd_sync_environment)
    wm.sculpt_gpu_smooth_a1 = BoolProperty(
        name="GPU Smooth A1",
        default=False,
        description="Use the experimental NVIDIA Warp GPU Smooth validation path",
        update=_xpbd_sync_environment)
    wm.sculpt_gpu_smooth_mode = EnumProperty(
        name="Smooth Mode",
        items=(
            ('NORMAL', "Normal", "Fast density-normalized smoothing"),
            ('RELAX', "Relax", "Shape-aware tangential and normal relaxation"),
            ('VOLUME', "Preserve Volume", "Smooth while strongly attenuating surface shrinkage"),
            ('STRONG', "Very Strong", "D12 local multiscale smoothing; CUDA when available, CPU chart otherwise"),
        ),
        default='NORMAL',
        update=_xpbd_sync_environment)
    wm.sculpt_physical_clay_compression = FloatProperty(
        name="Compression Resistance", min=0.0, max=1.0, default=0.65,
        update=_xpbd_sync_environment)
    wm.sculpt_physical_clay_material = EnumProperty(
        name="Material",
        items=(
            ('CLAY', "Clay", "Balanced cohesive elastoplastic material"),
            ('WAX', "Wax", "Stiffer material with precise deformation and recovery"),
            ('MUD', "Mud", "Soft, highly plastic material with stronger tangential flow"),
        ),
        default='CLAY',
        update=_xpbd_sync_environment)
    wm.sculpt_physical_clay_plasticity = FloatProperty(
        name="Plasticity", min=0.0, max=1.0, default=0.60,
        update=_xpbd_sync_environment)
    wm.sculpt_physical_clay_drag = FloatProperty(
        name="Tangential Drag", min=0.0, max=1.0, default=0.35,
        update=_xpbd_sync_environment)
    wm.sculpt_physical_clay_volume = FloatProperty(
        name="Volume Retention", min=0.0, max=1.0, default=0.50,
        update=_xpbd_sync_environment)
    wm.sculpt_physical_clay_recovery = FloatProperty(
        name="Elastic Recovery", min=0.0, max=1.0, default=0.20,
        update=_xpbd_sync_environment)
    wm.sculpt_physical_balloon = BoolProperty(
        name="Physical Balloon", default=False,
        description="Use a coherent low-frequency pressure field while preserving surface detail",
        update=_xpbd_sync_environment)
    wm.sculpt_balloon_pressure = FloatProperty(
        name="Pressure", min=0.0, max=2.0, default=1.0,
        update=_xpbd_sync_environment)
    wm.sculpt_balloon_tension = FloatProperty(
        name="Surface Tension", min=0.0, max=1.0, default=0.65,
        update=_xpbd_sync_environment)
    wm.sculpt_lesion_preview = BoolProperty(
        name="Lesion Preview", default=False,
        description="Open a surface incision without changing mesh topology",
        update=_xpbd_sync_environment)
    wm.sculpt_lesion_opening = FloatProperty(
        name="Opening", min=0.0, max=1.0, default=0.35,
        update=_xpbd_sync_environment)
    wm.sculpt_roll_alpha = BoolProperty(
        name="Roll Alpha", default=False,
        description="Move the Area texture continuously along the standard sculpt stroke",
        update=_xpbd_sync_environment)
    wm.sculpt_roll_repeat = FloatProperty(
        name="Roll Repeat", min=0.05, max=20.0, default=1.0,
        description="Texture phase cycles per brush radius travelled",
        update=_xpbd_sync_environment)


def _unregister_xpbd_runtime_settings():
    wm = bpy.types.WindowManager
    for name in (
            'sculpt_xpbd_grab_solver',
            'sculpt_xpbd_backend',
            'sculpt_xpbd_physical_assist',
            'sculpt_xpbd_material_compliance',
            'sculpt_xpbd_velocity_damping',
            'sculpt_physical_clay_v01',
            'sculpt_gpu_smooth_a1',
            'sculpt_gpu_smooth_mode',
            'sculpt_physical_clay_compression',
            'sculpt_physical_clay_material',
            'sculpt_physical_clay_plasticity',
            'sculpt_physical_clay_drag',
            'sculpt_physical_clay_volume',
            'sculpt_physical_clay_recovery',
            'sculpt_physical_balloon',
            'sculpt_balloon_pressure',
            'sculpt_balloon_tension',
            'sculpt_lesion_preview',
            'sculpt_lesion_opening',
            'sculpt_roll_alpha',
            'sculpt_roll_repeat'):
        if hasattr(wm, name):
            delattr(wm, name)

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
    os.environ.setdefault(
        'BLENDER_SCULPT_WARP_PTX',
        str(Path(bpy.app.binary_path).with_name('blender_xpbd_jacobi.sm75.ptx')))
    os.environ.setdefault(
        'BLENDER_SCULPT_WARP_SMOOTH_PTX',
        str(Path(bpy.app.binary_path).with_name('blender_smooth_a3.sm75.ptx')))
    os.environ.setdefault(
        'BLENDER_SCULPT_WARP_MULTISCALE_PTX',
        str(Path(bpy.app.binary_path).with_name('blender_smooth_multiscale.sm75.ptx')))
    os.environ.setdefault(
        'BLENDER_SCULPT_WARP_MULTISCALE_SYMBOLS',
        str(Path(bpy.app.binary_path).with_name('blender_smooth_multiscale.sm75.symbols')))
    _register_xpbd_runtime_settings()
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
    _unregister_xpbd_runtime_settings()
