# Polypaint UI continuation — 20 October 2026

Baseline: `release/sculpt-extended-native-20261020` at
`4259a8472d3a8e99a09eea2acefe62a59f24a617`. This change completes the
Polypaint controls available in that baseline; it does not reconstruct the later
Windows Bake Visible / Regional V03 source state.

## Available controls

The Multires sidebar now exposes Polypaint HR in Sculpt Mode and Color Attribute
HR in Object/Sculpt Mode, on meshes with exactly one Multires modifier. Both new
panels call the same drawing functions as the existing Sculpt Extended panels:

- Paint, Blur, Smear activation; Strength and Size.
- Paint Blend, Flow, Wet Mix, Wet Persistence, Density; Smear mode.
- Color Attribute selection/removal, residual HR master removal, and confirmed
  removal of all colors through the existing native operator and Undo call.

The existing Multires Polypaint → Texture UV panel is unchanged, including
attribute/UV choice, resolution, antialiasing, tile selection, margin, UDIM
series, saving and result viewing. Face Sets, Sculpt Layers, geometry kernels,
Draw Transport, native defaults, bake and Undo implementations are unchanged.
This is not a claim that later, unrecovered sidebar panels are complete.

The original native release kit under `../native_release` remains a historical,
hash-checked payload. Do not replay it to deploy this newer UI; its different-file
guard correctly rejects a newer module. Use the current source module for the
next complete release after recovering its verified source chain.

## Focused validation

From the repository root:

```sh
python3 tools/sculpt_extended/polypaint_ui_20261020/test_ui.py
blender --background --factory-startup --python-exit-code 1 --python tools/sculpt_extended/polypaint_ui_20261020/test_ui.py
```

Python: 13 checks passed, 1 Blender-specific check skipped. Blender 4.3.2 on the
cloud machine: all 14 checks passed, including actual class registration,
property lifecycle, Multires eligibility and standard Brush RNA properties.
Drawing/dispatch checks use layout and context doubles; they check both tabs,
tool-specific controls, empty/no brush states, selected/residual attributes,
confirmation context and native Undo dispatch. They do not execute native
Polypaint, paint strokes, the destructive clear operation or viewport drawing.
The Blender probe creates only a temporary cube in a factory-startup process.

The recorded Blender log is `RESULTS-blender-4.3.2.txt`. No new Windows
executable was built or certified by this change.

## Launch profile and outstanding recovery

`PROFILE.json` preserves the remote baseline's 27 feature settings and explicitly
sets `BLENDER_SCULPT_COMMIT_KEEP_CCG=0`. It is a baseline development profile, not
the unrecovered complete profile of the later verified Windows build. Existing
native startup still honors explicit environment overrides. Regional V03 must
remain a separate opt-in until its source payload has been recovered; Shared-Work
V03 must stay off. No new optimization defaults are installed here.

`RESUME_STATE.json` preserves the user's handoff, file IDs, executable checksum,
reported PASS results and constraints. These Windows results are user-supplied
evidence, not reruns in this environment. The fork's last verified remote commit
does not include the later Regional V03 and Bake Visible/Hidden payload chain.
The Library IDs cannot be downloaded with the tools available in this session.
Recover the baseline and full payload/manifest chain before merging or claiming
that all verified Windows changes have been published. Never copy the restored
shareable source tree over this branch as a substitute.

After that recovery: validate the combined UI on the existing verified Windows
portable, preserve the baseline/history, publish the recovered verified changes,
then start a separate optimization version with per-phase timings and peak RAM
measurements on dense meshes. Do not claim a general 3× speedup from the earlier
variable L5 timings.
