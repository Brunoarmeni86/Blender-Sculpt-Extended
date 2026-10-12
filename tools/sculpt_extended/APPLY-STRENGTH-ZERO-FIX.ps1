param(
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable'
)
$ErrorActionPreference = 'Stop'
$patch = Join-Path $PSScriptRoot 'layer-strength-zero.patch'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di installare.' }
if (!(Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) { throw 'Build configurata mancante.' }
& git.exe -C $SourceDir apply --check $patch
if ($LASTEXITCODE) { throw 'Patch non compatibile: nessun file modificato. Incolla il primo errore Git.' }
$source = Join-Path $SourceDir 'source\blender\makesrna\intern\rna_modifier.cc'
$backup = Join-Path 'C:\BlenderDev\Backups' ('strength-zero-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath $source -Destination (Join-Path $backup 'rna_modifier.cc')
& git.exe -C $SourceDir apply $patch
if ($LASTEXITCODE) { throw 'Applicazione patch fallita.' }
& cmake.exe --build $BuildDir --config Release --target INSTALL --parallel
if ($LASTEXITCODE) { throw 'Compilazione fallita: incolla il primo errore C++.' }
& (Join-Path $PSScriptRoot 'START-LAYER-CACHE-TEST.ps1') -BlenderExe (Join-Path $BuildDir 'bin\Release\blender.exe')
