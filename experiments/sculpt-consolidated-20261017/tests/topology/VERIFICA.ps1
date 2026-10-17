param([string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe')
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima della verifica.' }
if (Get-Process -Name cmake,cl,link,ninja,nvcc -ErrorAction SilentlyContinue) { throw 'Attendi il termine della compilazione corrente.' }
if (!(Test-Path -LiteralPath $BlenderExe)) { throw 'blender.exe non trovato.' }
$saved = @{}
$flags = @{
    BLENDER_MULTIRES_FACESETS_DRAW_V01 = '1'
    BLENDER_MULTIRES_FACESETS_VISIBILITY_V02 = '1'
    BLENDER_MULTIRES_FACESETS_GESTURES_V03 = '1'
    BLENDER_SCULPT_DETAIL_TRANSFER_V01 = '1'
    BLENDER_SCULPT_SUBDIVIDE_COMPACT = '1'
    BLENDER_SCULPT_LAYER_UI_UNDO = '1'
    BLENDER_SCULPT_LAYER_UNDO_CONTROLS = '1'
    BLENDER_SCULPT_LAYER_UNDO_CCG = '1'
    BLENDER_SCULPT_LAYER_UNDO_BOUNDARY = '0'
    BLENDER_SCULPT_LAYER_UNDO_METADATA = '0'
}
foreach ($name in $flags.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $flags.Keys) { [Environment]::SetEnvironmentVariable($name, $flags[$name], 'Process') }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $log = Join-Path $PSScriptRoot ("verifica-$stamp.log")
    $errors = Join-Path $PSScriptRoot ("verifica-$stamp-errors.log")
    $script = Join-Path $PSScriptRoot 'TEST-IN-BLENDER.py'
    $result = Join-Path $PSScriptRoot 'risultati-binding-v02.json'
    if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result }
    $process = Start-Process -FilePath $BlenderExe -ArgumentList @('--factory-startup','--python-exit-code','1','--python',('"' + $script + '"')) -RedirectStandardOutput $log -RedirectStandardError $errors -PassThru
    Write-Host "PID=$($process.Id). Verifica su scena nuova, senza aprire i tuoi file."
    Write-Host "LOG: $log"
    Write-Host "ERRORI: $errors"
    Write-Host 'Esito in risultati-binding-v02.json; scena demo-binding-v02.blend. Blender resta aperto se il test termina.'
}
finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
