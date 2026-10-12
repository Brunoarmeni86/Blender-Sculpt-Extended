param(
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [ValidateSet('Both','Commit','Subdivide','Off')][string]$Mode = 'Both',
    [switch]$ForceGlobalCommit
)
$ErrorActionPreference = 'Stop'
$env:BLENDER_SCULPT_COMMIT_SKIP_ORIGINALS = '0'
$env:BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE = '0'
if ($Mode -in @('Both','Commit')) { $env:BLENDER_SCULPT_COMMIT_SKIP_ORIGINALS = '1' }
if ($Mode -in @('Both','Subdivide')) { $env:BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE = '1' }
if ($ForceGlobalCommit) { $env:BLENDER_SCULPT_LOCAL_COMMIT = '0' }
else { Remove-Item Env:BLENDER_SCULPT_LOCAL_COMMIT -ErrorAction SilentlyContinue }
$env:BLENDER_LAYER_RESP_TRACE = '1'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $PSScriptRoot "commit-subdivide-$stamp.log"
Start-Process -FilePath $BlenderExe -RedirectStandardOutput $log -RedirectStandardError (Join-Path $PSScriptRoot "commit-subdivide-$stamp-errors.log")
Write-Host "Mode=$Mode ForceGlobalCommit=$ForceGlobalCommit; log=$log"
if ($ForceGlobalCommit) { Write-Host 'Solo su una scena di prova piccola: Sculpt al livello massimo, REC, pennellata, Commit. Verifica poi forma e Undo, chiudi Blender per completare i log.' }
