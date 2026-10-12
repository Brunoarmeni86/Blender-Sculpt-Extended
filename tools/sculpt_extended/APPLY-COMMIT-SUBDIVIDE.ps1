param(
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [switch]$Revert
)
$ErrorActionPreference = 'Stop'
$patch = Join-Path $PSScriptRoot 'commit-subdivide-combined.patch'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di installare.' }
if (!(Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) { throw 'Build configurata mancante.' }
$applyArgs = @('-C',$SourceDir,'apply')
if ($Revert) { $applyArgs += '-R' }
& git.exe @applyArgs --check $patch
if ($LASTEXITCODE) { throw 'Patch incompatibile: nessun file modificato. Incolla il primo errore Git.' }
$backup = Join-Path 'C:\BlenderDev\Backups' ('commit-subdivide-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath (Join-Path $SourceDir 'source\blender\blenkernel\intern\multires_reshape.cc') -Destination (Join-Path $backup 'multires_reshape.cc')
& git.exe @applyArgs $patch
if ($LASTEXITCODE) { throw 'Applicazione patch fallita.' }
& cmake.exe --build $BuildDir --config Release --target INSTALL --parallel
if ($LASTEXITCODE) { throw 'Compilazione fallita: incolla il primo errore C++.' }
$mode = 'Both'
if ($Revert) { $mode = 'Off' }
& (Join-Path $PSScriptRoot 'START-MEMORY-TEST.ps1') -BlenderExe (Join-Path $BuildDir 'bin\Release\blender.exe') -Mode $mode
Write-Host "Backup: $backup"
