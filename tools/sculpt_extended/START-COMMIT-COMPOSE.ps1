param(
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [switch]$Off
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di avviare la prova.' }
$env:BLENDER_SCULPT_COMMIT_SKIP_COMPOSE = '1'
if ($Off) { $env:BLENDER_SCULPT_COMMIT_SKIP_COMPOSE = '0' }
$env:BLENDER_SCULPT_COMMIT_SKIP_ORIGINALS = '1'
$env:BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE = '1'
$env:BLENDER_SCULPT_LOCAL_COMMIT = '0'
$env:BLENDER_LAYER_RESP_TRACE = '1'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $PSScriptRoot "commit-compose-$stamp.log"
Start-Process -FilePath $BlenderExe -RedirectStandardOutput $log -RedirectStandardError (Join-Path $PSScriptRoot "commit-compose-$stamp-errors.log")
Write-Host "SkipCompose=$env:BLENDER_SCULPT_COMMIT_SKIP_COMPOSE; log=$log"
Write-Host 'Prova su una copia piccola: massimo Sculpt Level, REC, pennellata, Commit, Undo/Redo. Chiudi Blender per completare i log.'
Write-Host 'Questo avvio forza il Commit globale per la prova. Gli altri launcher mantengono la propria configurazione.'
