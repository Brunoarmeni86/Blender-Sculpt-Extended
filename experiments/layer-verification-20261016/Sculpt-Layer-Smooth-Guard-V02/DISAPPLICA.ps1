param(
    [string]$SourceRoot = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable'
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'APPLICA.ps1') -SourceRoot $SourceRoot -BuildDir $BuildDir -RemovePatch
