param(
  [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_conform_test',
  [ValidateSet(1,2)][int]$Level = 1
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $BuildDir 'bin\Release\blender.exe'
if (!(Test-Path -LiteralPath $exe)) { throw "Eseguibile mancante: $exe" }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$log = Join-Path $PSScriptRoot ("anchor-$stamp.log")
$errors = Join-Path $PSScriptRoot ("anchor-$stamp.errors.log")
$previousDiag = $env:BLENDER_ANCHOR_DIAGNOSTICS
$previousV5 = $env:BLENDER_ANCHOR_PROJECTION_V5
$previousV52 = $env:BLENDER_ANCHOR_PROJECTION_V52
try {
  $env:BLENDER_ANCHOR_DIAGNOSTICS = [string]$Level
  $env:BLENDER_ANCHOR_PROJECTION_V5 = '1'
  $env:BLENDER_ANCHOR_PROJECTION_V52 = '1'
  Write-Host "Log: $log"
  Write-Host 'Prova Anchored su Multires, ridimensiona e prova la simmetria. Chiudi Blender per ottenere il riepilogo.'
  # Keep native stderr separate from PowerShell's pipeline error conversion.
  & $exe 2> $errors | Tee-Object -FilePath $log
  $blenderExit = $LASTEXITCODE
}
finally {
  $env:BLENDER_ANCHOR_DIAGNOSTICS = $previousDiag
  $env:BLENDER_ANCHOR_PROJECTION_V5 = $previousV5
  $env:BLENDER_ANCHOR_PROJECTION_V52 = $previousV52
}
if ($blenderExit -ne 0) { throw "Blender terminato con codice $blenderExit. Log errori: $errors" }
# Prefer Blender's bundled Python; no pip or extra packages needed.
$python = $null
$exeDir = Split-Path $exe -Parent
foreach ($dir in Get-ChildItem -LiteralPath $exeDir -Directory) {
  $candidate = Join-Path $dir.FullName 'python\bin\python.exe'
  if (Test-Path -LiteralPath $candidate) { $python = $candidate; break }
}
if (!$python) {
  Write-Host "Log salvato. Riepilogo manuale: python analizza_anchor.py `"$log`""
  return
}
& $python (Join-Path $PSScriptRoot 'analizza_anchor.py') $log
if ($LASTEXITCODE -ne 0) { throw "Riepilogo non generato. Conserva il log: $log" }
