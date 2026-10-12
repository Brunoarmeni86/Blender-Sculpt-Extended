param(
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [switch]$Revert
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di installare.' }
if (!(Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) { throw 'Build configurata mancante.' }
$patch = Join-Path $PSScriptRoot 'commit-work-parallel.patch'
$applyArgs = @('-C',$SourceDir,'apply')
if ($Revert) { $applyArgs += '-R' }
& git.exe @applyArgs --check $patch
if ($LASTEXITCODE) { throw 'Patch incompatibile: nessun file modificato. Incolla il primo errore Git.' }
$backup = Join-Path 'C:\BlenderDev\Backups' ('commit-parallel-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath (Join-Path $SourceDir 'source\blender\blenkernel\intern\multires.cc') -Destination (Join-Path $backup 'multires.cc')
& git.exe @applyArgs $patch
if ($LASTEXITCODE) { throw 'Applicazione patch fallita.' }
& cmake.exe --build $BuildDir --config Release --target INSTALL --parallel
if ($LASTEXITCODE) { throw 'Compilazione fallita: incolla il primo errore C++.' }
& (Join-Path $PSScriptRoot 'START-COMMIT-PARALLEL.ps1') -BlenderExe (Join-Path $BuildDir 'bin\Release\blender.exe') -Off:$Revert
Write-Host "Backup: $backup"
