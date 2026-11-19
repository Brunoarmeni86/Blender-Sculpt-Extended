# Native Vector Tangent frame correction: Windows diagnostic pass

User-supplied Blender 5.2.1 LTS report after production candidate df34194.
SS=4, Vector frame flag=1, VectorFrameProbe active, 120 finite nonzero samples,
including 60 regular and 60 mirrored UV cases. Frame hash and face/pixel pairs
match the pre-patch report. The uploaded report does not include a binary hash
or compiled source revision; association uses the incremental patch workflow.

| Native Tangent decoded through Cycles vs Object control | Before | After |
| --- | ---: | ---: |
| Relative vector RMS (%) | 2.679711569 | 0.000016123 |
| Amplitude error RMS (%) | 0.914050332 | 0.000008013 |
| Position error RMS (Blender units) | 0.004665195 | 0.000000026862 |

The report's mathutils float angle prints zero after the patch. Independent
float64 atan2(cross magnitude, dot) on uploaded object/decoded vectors resolves
a nonzero residual around a few millionths of a degree; do not claim exact zero.
Position error recalculation agrees exactly. Maximum Object control component
change between runs is 1.1921e-7, consistent with float precision. Both regular
and mirrored UV probes reach approximately float-precision agreement.

This is a native test of newly baked Tangent-map conversion, not just the
analytical Python inverse. It supports retaining the limited Vector frame fix.
No broader test or source change required for this evidence commit. The prior
mixed screen-space reconstruction results remain unresolved; this does not
certify seams, custom/split normals, object transforms, texture filtering or
complete Cycles geometry reconstruction. Main stays frozen; PR remains draft.
