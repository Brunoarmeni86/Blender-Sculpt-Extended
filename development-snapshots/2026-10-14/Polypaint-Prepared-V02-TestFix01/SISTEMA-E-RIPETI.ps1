param(
    [string]$KitDir = 'C:\BlenderDev\Polypaint-Prepared-V02',
    [string]$RunDir = '',
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [ValidateRange(30,3600)][int]$SmokeTimeout = 480
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di ripetere i test.' }
if (!(Test-Path -LiteralPath (Join-Path $KitDir 'COMPLETA-BUILD.ps1'))) { throw "Kit Prepared V02 mancante: $KitDir" }
if (!$RunDir) {
    $state = Join-Path $KitDir 'last-prepared-run.json'
    if (!(Test-Path -LiteralPath $state)) { throw 'Specifica -RunDir con la cartella risultati della compilazione gia completata.' }
    $RunDir = (Get-Content -LiteralPath $state -Raw | ConvertFrom-Json).run_dir
}
if (!(Test-Path -LiteralPath (Join-Path $RunDir 'compiled-binary.json'))) { throw "Compilazione completata non registrata in $RunDir" }
$backup = Join-Path $KitDir ('test-fix-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
foreach ($name in @('VERIFICA-PAINT-UI.py','VERIFICA-MULTI-UI.py','validation_brush.py')) {
    $target = Join-Path $KitDir $name
    if (Test-Path -LiteralPath $target) { Copy-Item -LiteralPath $target -Destination (Join-Path $backup $name) }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $target -Force
}
Write-Host "Aggiornati solo i test Python. Backup: $backup"
Write-Host "Ripeto i test sulla compilazione esistente: $RunDir"
& (Join-Path $KitDir 'COMPLETA-BUILD.ps1') -RunDir $RunDir -SourceDir $SourceDir -BuildDir $BuildDir -SmokeTimeout $SmokeTimeout
