"""Static OFF-by-default native V15 source-wiring checks.

This does NOT compile the guarded Blender source, nor validate viewport,
Multires, Sculpt Undo or CUDA. It catches missing wiring and accidental
unconditional activation of the research callbacks.
"""
from pathlib import Path

root=Path(__file__).resolve().parents[2]
sculpt=root/"source/blender/editors/sculpt_paint"
physics=root/"experimental/sculpt_physics_cpu"

build=(sculpt/"CMakeLists.txt").read_text(encoding="utf-8")
assert 'option(WITH_SCULPT_XPBD_OTHER_BRUSHES_V15' in build
assert 'WITH_SCULPT_XPBD_OTHER_BRUSHES_V15=ON' not in build
assert 'if(NOT WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14)' in build
assert 'PRIVATE WITH_SCULPT_XPBD_OTHER_BRUSHES_V15' in build
assert "native_grab_executor_adapter.cc" in build

bridge=(sculpt/"mesh/brushes/physics_brush_v15.hh").read_text(encoding="utf-8")
assert "#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15" in bridge
assert 'BLENDER_SCULPT_XPBD_OTHER_BRUSHES_V15' in bridge
assert 'prepare_cpu_native_brush_dab' in bridge
assert 'clip_and_lock_translations' in bridge
assert 'xpbd_v15_state' in bridge
assert 'covered.size()!=changes.size()' in bridge

calls={
    "clay.cc": ("NativeSculptBrushKind::Clay","calc_closest_to_plane"),
    "clay_strips.cc": ("NativeSculptBrushKind::ClayStrips","apply_z_axis_factors"),
    "smooth.cc": ("NativeSculptBrushKind::Smooth","neighbor_data_average_mesh_check_loose"),
    "draw.cc": ("NativeSculptBrushKind::Draw","NativeSculptBrushKind::Nudge",
                "NativeSculptBrushKind::Gravity")
}
for name,fragments in calls.items():
    src=(sculpt/"mesh/brushes"/name).read_text(encoding="utf-8")
    assert '#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15' in src,name
    assert 'physics_v15::requested()' in src or 'try_smooth_physics_v15' in src,name
    assert 'physics_v15::try_apply' in src,name
    assert 'physics_v15::freeze_if_mode_changed' in src,name
    for token in fragments: assert token in src,(name,token)

adapter=(physics/"native_grab_executor_adapter.cc").read_text(encoding="utf-8")
for token in ("NativeSculptBrushKind::Clay","NativeSculptBrushKind::ClayStrips",
              "NativeSculptBrushKind::Smooth","NativeSculptBrushKind::Draw",
              "NativeSculptBrushKind::Nudge","NativeSculptBrushKind::Gravity",
              "topology.one_ring_halo","executor->stage_incremental"):
    assert token in adapter,token

print("V15 source wiring and default-OFF checks passed (static only)")
