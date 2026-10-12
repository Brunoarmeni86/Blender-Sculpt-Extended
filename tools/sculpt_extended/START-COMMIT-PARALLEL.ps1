param(
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [switch]$Off
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di avviare la prova.' }
$env:BLENDER_SCULPT_COMMIT_WORK_PARALLEL = '1'
if ($Off) { $env:BLENDER_SCULPT_COMMIT_WORK_PARALLEL = '0' }
$env:BLENDER_SCULPT_LOCAL_COMMIT = '1'
$env:BLENDER_LAYER_RESP_TRACE = '1'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $PSScriptRoot "commit-parallel-$stamp.log"
Start-Process -FilePath $BlenderExe -RedirectStandardOutput $log -RedirectStandardError (Join-Path $PSScriptRoot "commit-parallel-$stamp-errors.log")
Write-Host "WorkParallel=$env:BLENDER_SCULPT_COMMIT_WORK_PARALLEL; log=$log"
Write-Host 'Copia di scena: livello massimo, REC su layer visibile, pennellata abbastanza ampia, Commit, Undo/Redo. Chiudi Blender per completare i log.'
Write-Host 'Il Commit locale e attivo; niente modifica ai flag di cache, fast tangent o publish.'
