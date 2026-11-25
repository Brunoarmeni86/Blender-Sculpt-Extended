# V27 material library bridge — V0.6
The original Material Brushes V0.6 has 31 recipes across Clay, Flesh, Fabric, Stone, Wax, Wood, Mud and Foam. This migration starts with the real original assets (saved blend or trusted original ZIP), not recreated icons. Originals remain untouched. All 31 distinct recipe IDs must be present before export. Artist variants in a source blend are retained separately.

## Build on the user's Windows installation
```powershell
$exe = "C:\BlenderDev\install-physics-v16\blender.exe"
$script = "C:\BlenderDev\blender-physics-v16\experimental\sculpt_physics_cpu\adapt_material_brushes_v27.py"
& $exe --factory-startup --background --python-exit-code 1 --python $script -- --self-test
& $exe --factory-startup --background --python-exit-code 1 --python $script -- --addon-zip "C:\path\Sculpt_Extended_Material_Brushes_V0.6.zip" --output "C:\BlenderDev\assets\BSE-Material-V27"
```
Alternatively replace --addon-zip with --source-blend pointing to a saved project with all 31 managed V0.6 assets. Add the output directory in Preferences > File Paths > Asset Libraries. Keep the V0.6 library registered for A/B.

## Scope and limits
Known recipes route to existing MATERIAL actions; additional supported native brush types route to ASSISTED. Native references are explicitly marked rather than pretending that Cloth, Elastic, stamps, fine grooves, or fracture have an implemented new solver. The JSON report lists every actual recipe and routing. Wax Knife / Wood Gouge / Stone Chisel material variants are approximate single-plane carving; original narrow Crease and Multiplane tools remain available. Rock Polish is a trial of surface relaxation, not true abrasive removal. Deposit is clamped to strength 1 by the current V23 control path; do not present this as a replacement for the original Clay Build strength 3. Texture/preview identity is copied; changing sculpt type can alter the effective footprint. Catalog metadata is copied if present. Per-material rheology remains process-wide via environment, not per brush. No Blender execution, runtime or actual asset generation performed in this turn.

## Required next gates
- Verify executable contains V27C diagnostics; prior log contains no V27C region records.
- Artist A/B across low/high density, overlap, masks, inversions, pressure and Undo.
- Implement swept PBVH, local plastic history, conservative directional transport and GPU executor; not implemented by asset migration.
- Implement true narrow cutting / multiplane chisel, wood anisotropy, abrasion, cloth/foam/granular modes before claiming all 31 physically adapted.
- Preserve original native and Multires workflows; current MATERIAL solver is restricted to supported ordinary Mesh topology.
