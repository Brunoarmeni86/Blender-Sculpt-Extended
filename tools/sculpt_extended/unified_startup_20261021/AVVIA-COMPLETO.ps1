param(
    [string]$BlenderExe = (Join-Path (Split-Path $PSScriptRoot) 'blender.exe'),
    [string]$BlendFile = '',
    [ValidateRange(0,8192)][int]$WeightCacheMB = 1024
)
$ErrorActionPreference = 'Stop'
# Compatibility entry point: native startup owns all feature defaults and UI.
$arguments = @()
if ($BlendFile) { $arguments += $BlendFile }
$oldCache = [Environment]::GetEnvironmentVariable('BLENDER_SCULPT_WEIGHT_MULTI_CACHE_MB','Process')
try {
    if ($PSBoundParameters.ContainsKey('WeightCacheMB')) {
        [Environment]::SetEnvironmentVariable('BLENDER_SCULPT_WEIGHT_MULTI_CACHE_MB',[string]$WeightCacheMB,'Process')
    }
    & $BlenderExe @arguments
} finally {
    [Environment]::SetEnvironmentVariable('BLENDER_SCULPT_WEIGHT_MULTI_CACHE_MB',$oldCache,'Process')
}
