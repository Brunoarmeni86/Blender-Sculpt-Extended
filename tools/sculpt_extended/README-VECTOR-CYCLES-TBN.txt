Extract to C:\BlenderDev\Bake-Vector-Cycles-TBN.
Keep the installed CPU UV coverage and Normal HQ patches.
Close Blender and use existing Developer PowerShell:

Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& 'C:\BlenderDev\Bake-Vector-Cycles-TBN\APPLY-VECTOR-CYCLES-TBN.ps1'

Incremental compile + INSTALL using build_windows_x64_shareable; no CMake reset.
Close Blender opened after installation, then run the short two-bake audit:
& 'C:\BlenderDev\Bake-Vector-Cycles-TBN\TEST-VECTOR-CANDIDATE.ps1'

Return native-vector-frame-candidate.json. Native candidate is not tested yet.

Revert only this patch + rebuild:
& 'C:\BlenderDev\Bake-Vector-Cycles-TBN\APPLY-VECTOR-CYCLES-TBN.ps1' -Revert

Diagnostic old frame in a fresh Blender process:
$env:BLENDER_MULTIRES_BAKE_VECTOR_CYCLES_TBN = '0'
