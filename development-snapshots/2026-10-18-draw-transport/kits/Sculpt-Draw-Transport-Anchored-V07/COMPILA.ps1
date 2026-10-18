param([string]$BuildRoot = 'C:\BlenderDev\build_windows_x64_shareable', [ValidateRange(1,64)][int]$Parallel = 4)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima della build.' }
$cache = Join-Path $BuildRoot 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cache)) { throw "Build non configurata: $BuildRoot" }
$command = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeExe = if ($command) { $command.Source } else { $null }
if (!$cmakeExe) {
    $entry = Get-Content -LiteralPath $cache | Where-Object { $_ -match '^CMAKE_COMMAND:INTERNAL=' } | Select-Object -First 1
    if ($entry) { $cmakeExe = ($entry -split '=',2)[1].Trim() }
}
if (!$cmakeExe -or !(Test-Path -LiteralPath $cmakeExe)) { throw 'CMake non trovato nel PATH o nel cache della build. Usa Developer PowerShell VS2022.' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$log = Join-Path $PSScriptRoot "build-draw-transport-$stamp.log"
$savedErrorPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & $cmakeExe --build $BuildRoot --config Release --parallel $Parallel 2>&1 | Tee-Object -FilePath $log
    $code = $LASTEXITCODE
} finally { $ErrorActionPreference = $savedErrorPreference }
Write-Host "Log build: $log"
if ($code -ne 0) { throw "Build fallita ($code). Carica il log; sorgenti precedenti nel backup." }
Write-Host 'BUILD COMPLETATA. Avvia AVVIA.ps1 per la prova.' -ForegroundColor Green
