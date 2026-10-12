param(
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [ValidateRange(0,32768)][int]$CacheMB = 512,
    [switch]$Revert
)
$ErrorActionPreference = 'Stop'
$patch = Join-Path $PSScriptRoot 'layer-cache-combined.patch'
if (!(Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) { throw 'Build esistente non configurata.' }
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di installare.' }
$argsApply = @('-C',$SourceDir,'apply')
if ($Revert) { $argsApply += '-R' }
& git.exe @argsApply --check $patch
if ($LASTEXITCODE) { throw 'Patch non compatibile con i sorgenti attuali: nessun file modificato. Incolla il primo errore Git.' }
$backup = Join-Path 'C:\BlenderDev\Backups' ('layer-cache-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
foreach ($relative in @('source\blender\blenkernel\intern\multires.cc','source\blender\blenkernel\intern\multires_sculpt_layers.cc','source\blender\blenkernel\BKE_multires_sculpt_layers.hh','source\blender\makesrna\intern\rna_modifier.cc','source\blender\blenkernel\intern\multires_layer_cache_budget.hh')) {
    $source = Join-Path $SourceDir $relative
    if (Test-Path -LiteralPath $source) {
        $dest = Join-Path $backup $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $dest
    }
}
& git.exe @argsApply $patch
if ($LASTEXITCODE) { throw 'Applicazione patch fallita.' }
& cmake.exe --build $BuildDir --config Release --target INSTALL --parallel
if ($LASTEXITCODE) { throw 'Compilazione fallita: incolla il primo errore C++. Nessun Blender avviato.' }
if ($Revert) {
    Remove-Item Env:BLENDER_SCULPT_LAYER_CACHE_MB -ErrorAction SilentlyContinue
    Remove-Item Env:BLENDER_SCULPT_LAYER_SUPPORT_CACHE -ErrorAction SilentlyContinue
    Start-Process (Join-Path $BuildDir 'bin\Release\blender.exe')
} else {
    & (Join-Path $PSScriptRoot 'START-LAYER-CACHE-TEST.ps1') -BlenderExe (Join-Path $BuildDir 'bin\Release\blender.exe') -CacheMB $CacheMB
}
Write-Host "Backup: $backup"
