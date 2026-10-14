param(
    [string]$BlenderExe = '',
    [string]$BlendFile = '',
    [ValidateRange(0, 32768)][int]$CacheMB = 512,
    [switch]$PublishOff,
    [switch]$CompactOff,
    [switch]$NoPolypaint,
    [switch]$UndoOff,
    [switch]$TestPolypaint
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di avviare la prova.' }
if (!$BlenderExe) {
    $paths = Join-Path $PSScriptRoot 'experiment-paths.json'
    if (!(Test-Path -LiteralPath $paths)) { throw 'Esegui prima APPLICA-SULLA-BUILD-ATTUALE.ps1.' }
    $BlenderExe = (Get-Content -LiteralPath $paths -Raw | ConvertFrom-Json).BlenderExe
}
if (!(Test-Path -LiteralPath $BlenderExe)) { throw "Blender mancante: $BlenderExe" }
if ($TestPolypaint -and ($BlendFile -or $NoPolypaint)) { throw 'TestPolypaint richiede Paint attivo e una scena di test nuova.' }
$profileVariables = @('BLENDER_LAYER_RESP_TRACE', 'BLENDER_SCULPT_COMMIT_KEEP_CCG', 'BLENDER_SCULPT_COMMIT_WORK_PARALLEL', 'BLENDER_SCULPT_COMPACT_COMMIT_STAGING', 'BLENDER_SCULPT_FIRST_STROKE_ANCHOR', 'BLENDER_SCULPT_LAYER_CACHE_MB', 'BLENDER_SCULPT_LAYER_READONLY_WEIGHT', 'BLENDER_SCULPT_LAYER_STACK_CHECK_V3', 'BLENDER_SCULPT_LOCAL_COMMIT', 'BLENDER_SCULPT_SKIP_EMPTY_BM_FLUSH', 'BLENDER_SCULPT_UNDO_BOUNDARY_METADATA', 'BLENDER_SCULPT_COMMIT_PUBLISH_SMALL_GRIDS', 'BLENDER_SCULPT_POLYPAINT_CPU', 'BLENDER_SCULPT_PARALLEL_PUBLISH')
$savedProfile = @{}
foreach ($name in $profileVariables) { $savedProfile[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:BLENDER_SCULPT_LAYER_CACHE_MB = [string]$CacheMB
    $env:BLENDER_SCULPT_COMPACT_COMMIT_STAGING = '1'
    $env:BLENDER_SCULPT_FIRST_STROKE_ANCHOR = '1'
    $env:BLENDER_SCULPT_COMMIT_KEEP_CCG = '1'
    $env:BLENDER_SCULPT_SKIP_EMPTY_BM_FLUSH = '1'
    $env:BLENDER_SCULPT_UNDO_BOUNDARY_METADATA = '1'
    $env:BLENDER_SCULPT_LAYER_STACK_CHECK_V3 = '1'
    $env:BLENDER_SCULPT_LAYER_READONLY_WEIGHT = '1'
    $env:BLENDER_SCULPT_COMMIT_WORK_PARALLEL = '1'
    $env:BLENDER_SCULPT_LOCAL_COMMIT = '1'
    $env:BLENDER_LAYER_RESP_TRACE = '1'
    $env:BLENDER_SCULPT_COMMIT_PUBLISH_SMALL_GRIDS = '1'
    $env:BLENDER_SCULPT_PARALLEL_PUBLISH = '1'
    $env:BLENDER_SCULPT_POLYPAINT_CPU = '1'
    if ($PublishOff) { $env:BLENDER_SCULPT_COMMIT_PUBLISH_SMALL_GRIDS = '0' }
    if ($CompactOff) { $env:BLENDER_SCULPT_COMPACT_COMMIT_STAGING = '0' }
    if ($NoPolypaint) { $env:BLENDER_SCULPT_POLYPAINT_CPU = '0' }
    if ($UndoOff) {
        $env:BLENDER_SCULPT_FIRST_STROKE_ANCHOR = '0'
        $env:BLENDER_SCULPT_UNDO_BOUNDARY_METADATA = '0'
    }
    $mode = 'p' + $env:BLENDER_SCULPT_COMMIT_PUBLISH_SMALL_GRIDS + '-c' + $env:BLENDER_SCULPT_COMPACT_COMMIT_STAGING + '-u' + $env:BLENDER_SCULPT_FIRST_STROKE_ANCHOR + '-paint' + $env:BLENDER_SCULPT_POLYPAINT_CPU
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $log = Join-Path $PSScriptRoot "sculpt-polypaint-next-$mode-$stamp.log"
    $errorLog = Join-Path $PSScriptRoot "sculpt-polypaint-next-$mode-$stamp-errors.log"
    $testScript = Join-Path $PSScriptRoot 'PROVA-POLYPAINT.py'
    $arguments = @()
    if ($BlendFile) {
        if (!(Test-Path -LiteralPath $BlendFile)) { throw "Scena mancante: $BlendFile" }
        $arguments += ('"' + $BlendFile + '"')
    }
    if ($TestPolypaint) { $arguments += '--factory-startup' }
    $arguments += @('--python', ('"' + $testScript + '"'))
    if ($TestPolypaint) { $arguments += @('--', '--polypaint-test') }
    $process = Start-Process -FilePath $BlenderExe -ArgumentList $arguments -RedirectStandardOutput $log -RedirectStandardError $errorLog -PassThru
    Write-Host "PID=$($process.Id); profilo=$mode; cache=$CacheMB MB"
    Write-Host "Log: $log"
    Write-Host "Errori: $errorLog"
    Write-Host 'Commit/Undo V4.1 attivi. PublishOff confronta la nuova pubblicazione; NoPolypaint disabilita Paint Multires.'
    Write-Host 'TestPolypaint prepara una sfera piccola con Color Attribute e materiale. Paint -> Undo -> Redo; livelli 3 -> 1 -> 3; salva e riapri.'
    Write-Host 'Chiudi Blender per completare i log.'
} finally {
    foreach ($name in $profileVariables) { [Environment]::SetEnvironmentVariable($name, $savedProfile[$name], 'Process') }
}
