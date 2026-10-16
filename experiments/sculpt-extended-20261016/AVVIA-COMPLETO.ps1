param(
    [string]$BlenderExe = (Join-Path (Split-Path $PSScriptRoot) 'blender.exe'),
    [string]$BlendFile = '',
    [ValidateRange(0,8192)][int]$WeightCacheMB = 1024
)
$ErrorActionPreference = 'Stop'
$outer = Join-Path $PSScriptRoot 'AVVIA-ESPERIMENTO.ps1'
if (!(Test-Path -LiteralPath $outer)) { throw "Launcher esterno non trovato: $outer" }
$flags = @{
    BLENDER_MULTIRES_STARTUP_REUSE = '1'
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
    BLENDER_MULTIRES_GEOMETRY_BUFFER = '1'
    BLENDER_LAYER_RESP_TRACE = '1'
    BLENDER_SCULPT_REFINE_PARALLEL = $null
}
$saved = @{}
foreach ($name in $flags.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $flags.Keys) { [Environment]::SetEnvironmentVariable($name, $flags[$name], 'Process') }
    & $outer -BlenderExe $BlenderExe -BlendFile $BlendFile -CacheMB 1024
}
finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}

