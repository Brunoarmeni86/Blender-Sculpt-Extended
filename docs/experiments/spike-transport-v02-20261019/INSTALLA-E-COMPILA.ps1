param(
    [string]$SourceRoot = 'C:\BlenderDev\blender',
    [string]$BuildRoot = 'C:\BlenderDev\build_windows_x64_shareable',
    [ValidateRange(1,64)][int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender.' }
$cache = Join-Path $BuildRoot 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cache)) { throw 'BuildRoot non valido.' }
$entry = Get-Content -LiteralPath $cache | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' } | Select-Object -First 1
if (!$entry) { throw 'CMAKE_HOME_DIRECTORY mancante.' }
$configured = [IO.Path]::GetFullPath(($entry -split '=',2)[1].Trim()).TrimEnd('\','/')
$requested = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\','/')
if ($configured -ine $requested) { throw "Build collegata a sorgenti diversi: $configured" }
$active = @(Get-CimInstance Win32_Process | Where-Object {
    ($_.Name -in @('cmake.exe','ninja.exe','cl.exe','link.exe')) -or
    ($_.Name -eq 'MSBuild.exe' -and $_.CommandLine -notmatch '(?i)/nodemode:1(?:\s|$)')
})
if ($active.Count) { throw 'Attendi la fine della compilazione attiva.' }
if (!(Get-Command python -ErrorAction SilentlyContinue)) { throw 'Python non trovato nel PATH.' }
& python (Join-Path $PSScriptRoot 'apply_spike_transport.py') $SourceRoot --dry-run
if ($LASTEXITCODE -ne 0) { throw 'Verifica compatibilita fallita; sorgenti invariati.' }
& python (Join-Path $PSScriptRoot 'apply_spike_transport.py') $SourceRoot
if ($LASTEXITCODE -ne 0) { throw 'Installazione fallita.' }
& (Join-Path $PSScriptRoot 'COMPILA.ps1') -BuildRoot $BuildRoot -Parallel $Jobs
Write-Host 'Compilazione completata. Usa AVVIA-TEST.ps1.' -ForegroundColor Green
