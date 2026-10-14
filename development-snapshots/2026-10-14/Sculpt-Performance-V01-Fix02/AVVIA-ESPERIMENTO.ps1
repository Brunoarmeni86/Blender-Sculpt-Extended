param(
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [string]$BlendFile = '',
    [string]$ScriptAvvio = (Join-Path $PSScriptRoot 'experiment-tools\AVVIA-ESPERIMENTO.ps1'),
    [ValidateRange(0, 32768)][int]$CacheMB = 512,
    [switch]$MeshColorOff,
    [switch]$SubdivideOff,
    [switch]$UndoProfile,
    [switch]$TestPolypaint,
    [switch]$PublishOff,
    [switch]$CompactOff,
    [switch]$NoPolypaint,
    [switch]$UndoOff
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $ScriptAvvio)) { throw "Avvio incluso nel kit mancante: $ScriptAvvio" }
if (!$BlenderExe) {
    $paths = Join-Path $PSScriptRoot 'experiment-paths.json'
    if (!(Test-Path -LiteralPath $paths)) { throw 'Esegui prima APPLICA-SULLA-BUILD-ATTUALE.ps1.' }
    $BlenderExe = (Get-Content -LiteralPath $paths -Raw | ConvertFrom-Json).BlenderExe
}
$flags = @{
    BLENDER_SCULPT_POLYPAINT_MESH_COLOR = $(if ($MeshColorOff) { '0' } else { '1' })
    BLENDER_SCULPT_SUBDIVIDE_PRESERVE_RETAINED = '1'
    BLENDER_SCULPT_SUBDIVIDE_COMPACT = $(if ($SubdivideOff) { '0' } else { '1' })
    BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE = '1'
    BLENDER_SCULPT_SUBDIVIDE_PROFILE = '1'
    BLENDER_SCULPT_UNDO_COMPRESSION_PROFILE = $(if ($UndoProfile) { '1' } else { '0' })
    BLENDER_SCULPT_LAYER_UNDO_TRACE = $(if ($UndoProfile) { '1' } else { '0' })
}
$saved = @{}
foreach ($name in $flags.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $flags.Keys) { [Environment]::SetEnvironmentVariable($name, $flags[$name], 'Process') }
    Write-Host "Suddivisione compatta: $env:BLENDER_SCULPT_SUBDIVIDE_COMPACT; precisione: 1; Undo profile: $env:BLENDER_SCULPT_UNDO_COMPRESSION_PROFILE"
    $parameters = @{
        BlenderExe = $BlenderExe; BlendFile = $BlendFile; CacheMB = $CacheMB
        TestPolypaint = [bool]$TestPolypaint; PublishOff = [bool]$PublishOff
        CompactOff = [bool]$CompactOff; NoPolypaint = [bool]$NoPolypaint; UndoOff = [bool]$UndoOff
    }
    Write-Host "Color Attribute sulla mesh valutata: $env:BLENDER_SCULPT_POLYPAINT_MESH_COLOR"
    & $ScriptAvvio @parameters
} finally {
    foreach ($name in $flags.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
