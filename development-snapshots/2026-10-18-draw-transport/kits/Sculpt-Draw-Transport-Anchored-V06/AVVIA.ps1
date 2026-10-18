param(
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [string]$BlendFile = '',
    [string]$SourceRoot = 'C:\BlenderDev\blender',
    [ValidateRange(0,8192)][int]$WeightCacheMB = 1024,
    [switch]$Reference,
    [switch]$NoCoherence,
    [switch]$Serial,
    [switch]$NoTransportProfile
)
$ErrorActionPreference = 'Stop'
$outer = Join-Path (Split-Path $BlenderExe) 'SculptExtended\AVVIA-ESPERIMENTO.ps1'
if (!(Test-Path -LiteralPath $outer)) { throw "Launcher esterno non trovato: $outer" }
$flags = @{
    BLENDER_SCULPT_DRAW_TRANSPORT_NO_COHERENCE = [string][int]$NoCoherence.IsPresent
    BLENDER_SCULPT_DRAW_TRANSPORT_REFERENCE = [string][int]$Reference.IsPresent
    BLENDER_SCULPT_DRAW_TRANSPORT_SERIAL = [string][int]$Serial.IsPresent
    BLENDER_SCULPT_DRAW_TRANSPORT_PROFILE = [string][int](-not $NoTransportProfile.IsPresent)
    BLENDER_SCULPT_DRAW_TRANSPORT_V01 = '1'
    BLENDER_SCULPT_DETAIL_TRANSFER_V01 = '1'
    BLENDER_MULTIRES_STARTUP_REUSE = '1'
    BLENDER_MULTIRES_CCG_FULL_BATCH = '1'
    BLENDER_SCULPT_OPEN_FILE_ANCHOR = '1'
    BLENDER_SCULPT_COLD_START_LOG = $null
    BLENDER_SCULPT_STROKE_PROFILE_LOG = $null
    BLENDER_MULTIRES_DRAW_PROFILE_LOG = $null
    BLENDER_MULTIRES_PBVH_GROUP_FACTOR = '1'
    BLENDER_MULTIRES_OVERLAY_NEUTRAL_SKIP = '1'
    BLENDER_SCULPT_WEIGHT_MULTI_CACHE = '1'
    BLENDER_SCULPT_WEIGHT_MULTI_CACHE_MB = [string]$WeightCacheMB
    BLENDER_SCULPT_WEIGHT_HOT_DELTA_CACHE = '1'
    BLENDER_SCULPT_WEIGHT_HOT_SKIP_STITCH = '0'
    BLENDER_SCULPT_COMMIT_FRAME_CACHE = '0'
    BLENDER_SCULPT_LAYER_BAKE_BASE = '1'
    BLENDER_SCULPT_SMOOTH_ISOLATED = '0'
    BLENDER_SCULPT_LAYER_DELTA_SMOOTH = '1'
    # Explicitly enable the existing V36 UI-property Undo route.
    BLENDER_SCULPT_LAYER_UNDO_CONTROLS = '1'
    BLENDER_SCULPT_LAYER_UI_UNDO = '1'
    BLENDER_SCULPT_LAYER_UNDO_CCG = '1'
    # Legacy metadata overlays must not override genuine property Undo.
    BLENDER_SCULPT_LAYER_UNDO_BOUNDARY = '0'
    BLENDER_SCULPT_LAYER_UNDO_METADATA = '0'

    BLENDER_MULTIRES_GEOMETRY_BUFFER = '1'
    BLENDER_LAYER_RESP_TRACE = '1'
    BLENDER_SCULPT_REFINE_PARALLEL = $null
}
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima della prova.' }
$session = [ordered]@{
    StartTime = (Get-Date).ToString('o')
    BlenderExe = $BlenderExe
    LogRoot = (Join-Path $PSScriptRoot 'experiment-tools')
}
$sourceFiles = @(
    'source/blender/blenkernel/intern/multires_sculpt_layers.cc',
    'source/blender/editors/sculpt_paint/mesh/sculpt_ops.cc',
    'source/blender/editors/sculpt_paint/mesh/sculpt.cc',
    'source/blender/editors/sculpt_paint/mesh/sculpt_draw_transport_v01.hh',
    'source/blender/blenkernel/BKE_multires_sculpt_layers.hh',
    'source/blender/blenkernel/intern/multires_detail_transfer_v01.hh',
    'source/blender/editors/sculpt_paint/mesh/sculpt_detail_transfer_v01.hh'
)
$sourceState = @()
foreach ($relative in $sourceFiles) {
    $path = Join-Path $SourceRoot $relative
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $content = [IO.File]::ReadAllText($path)
        $markers = @('DETAIL_TRANSFER_V01') | Where-Object { $content.Contains($_) }
        $sourceState += [ordered]@{
            Path = $relative; SHA256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            Markers = @($markers)
        }
    }
}
$session['SourceRoot'] = $SourceRoot
$session['SourceState'] = $sourceState
$session['ExecutableSHA256'] = (Get-FileHash -LiteralPath $BlenderExe -Algorithm SHA256).Hash
$session['UndoPolicy'] = 'V36 controls=1; UI Undo=1; CCG=1; legacy boundary=0; metadata overlay=0'
Write-Host 'Draw Transport V06: N > Multires > Draw Transport V06. Draw + Anchored.' -ForegroundColor Green
Write-Host 'Richiede la nuova patch C++ compilata. Conferma nel layer attivo; ESC annulla lo stroke.'
$session | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'ultima-prova.json') -Encoding UTF8
$saved = @{}
foreach ($name in $flags.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $flags.Keys) { [Environment]::SetEnvironmentVariable($name, $flags[$name], 'Process') }
    & $outer -BlenderExe $BlenderExe -BlendFile $BlendFile -CacheMB 1024 -ScriptAvvio (Join-Path $PSScriptRoot 'experiment-tools\AVVIA-ESPERIMENTO.ps1')
}
finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}


