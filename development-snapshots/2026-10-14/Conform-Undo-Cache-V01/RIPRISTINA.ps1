param(
    [Parameter(Mandatory=$true)][string]$RunDir,
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [string]$OutputDir = 'C:\BlenderDev\Experimental-Builds',
    [ValidateRange(1, 256)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$arguments = @{RunDir=$RunDir; SourceDir=$SourceDir; BuildDir=$BuildDir; OutputDir=$OutputDir; Jobs=$Jobs; Restore=$true}
& (Join-Path $PSScriptRoot 'COMPILA-E-VERIFICA.ps1') @arguments
