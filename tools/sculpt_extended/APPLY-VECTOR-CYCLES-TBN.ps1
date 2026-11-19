param(
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [switch]$Revert
)

$ErrorActionPreference = 'Stop'
$patch = Join-Path $PSScriptRoot 'multires-bake-vector-cycles-tbn.patch'
$relativeFile = 'source\blender\render\intern\multires_bake.cc'
$sourceFile = Join-Path $SourceDir $relativeFile
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $patch)) { throw "Patch mancante: $patch" }
if (!(Test-Path -LiteralPath $sourceFile)) { throw "Sorgente mancante: $sourceFile" }
if (!(Test-Path -LiteralPath $cache)) { throw "Build configurata mancante: $cache" }
if (Get-Process -Name blender -ErrorAction SilentlyContinue) {
    throw 'Chiudi Blender prima di applicare e installare la patch.'
}

$applyArgs = @('-C', $SourceDir, 'apply')
if ($Revert) { $applyArgs += '-R' }
& git.exe @applyArgs --check $patch
if ($LASTEXITCODE) { throw 'Patch incompatibile: nessuna modifica applicata.' }

$backup = Join-Path 'C:\BlenderDev\Backups' ('bake-vector-tbn-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup -ErrorAction Stop | Out-Null
Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $backup 'multires_bake.cc')
Write-Host "Backup: $backup"

& git.exe @applyArgs $patch
if ($LASTEXITCODE) { throw 'Applicazione patch fallita.' }
& cmake.exe --build $BuildDir --config Release --target INSTALL --parallel
if ($LASTEXITCODE) { throw 'Compilazione fallita. Nessun Blender avviato.' }

$env:BLENDER_MULTIRES_BAKE_SS = '4'
if ($Revert) {
    Remove-Item Env:BLENDER_MULTIRES_BAKE_VECTOR_CYCLES_TBN -ErrorAction SilentlyContinue
} else {
    $env:BLENDER_MULTIRES_BAKE_VECTOR_CYCLES_TBN = '1'
}
$exe = Join-Path $BuildDir 'bin\Release\blender.exe'
if (!(Test-Path -LiteralPath $exe)) { throw "Eseguibile mancante: $exe" }
Start-Process -FilePath $exe -ArgumentList '--factory-startup'
