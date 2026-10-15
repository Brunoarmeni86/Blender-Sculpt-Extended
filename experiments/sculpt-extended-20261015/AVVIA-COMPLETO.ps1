param(
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [string]$BlendFile = ''
)
$ErrorActionPreference = 'Stop'
$env:BLENDER_SCULPT_LAYER_BAKE_BASE = '1'
$env:BLENDER_SCULPT_SMOOTH_ISOLATED = '0'
$env:BLENDER_SCULPT_LAYER_DELTA_SMOOTH = '1'
$env:BLENDER_MULTIRES_GEOMETRY_BUFFER = '1'
$env:BLENDER_LAYER_RESP_TRACE = '1'
Remove-Item Env:\BLENDER_MULTIRES_DRAW_PROFILE_LOG -ErrorAction SilentlyContinue
Remove-Item Env:\BLENDER_SCULPT_STROKE_PROFILE_LOG -ErrorAction SilentlyContinue
Remove-Item Env:\BLENDER_SCULPT_REFINE_PARALLEL -ErrorAction SilentlyContinue
& (Join-Path $PSScriptRoot 'AVVIA-ESPERIMENTO.ps1') -BlenderExe $BlenderExe -BlendFile $BlendFile -CacheMB 1024