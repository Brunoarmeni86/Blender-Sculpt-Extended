param(
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [ValidateRange(0,32768)][int]$CacheMB = 512
)
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'SCULPT-LAYER-CACHE-BENCH.py'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$env:BLENDER_SCULPT_LAYER_CACHE_MB = [string]$CacheMB
$env:BLENDER_SCULPT_LAYER_SUPPORT_CACHE = '1'
$env:BLENDER_LAYER_RESP_TRACE = '1'
$stdout = Join-Path $PSScriptRoot "layer-cache-$stamp.log"
$stderr = Join-Path $PSScriptRoot "layer-cache-$stamp-errors.log"
Start-Process -FilePath $BlenderExe -ArgumentList '--factory-startup','--python',('"'+$script+'"') -RedirectStandardOutput $stdout -RedirectStandardError $stderr
Write-Host 'Apri una copia della scena di test, entra in Sculpt e fai una pennellata con REC sul layer.'
Write-Host 'F3: Sculpt Layer Cache Benchmark. Poi verifica strength/visibility/Undo, chiudi Blender e manda JSON + log.'
Write-Host "Log: $stdout"
