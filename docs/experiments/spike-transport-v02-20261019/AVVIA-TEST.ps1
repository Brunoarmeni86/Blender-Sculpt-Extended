param(
    [ValidateSet('rotation','native')][string]$Mode = 'rotation',
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [string]$BlendFile = ''
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $BlenderExe)) { throw 'blender.exe non trovato.' }
if ($BlendFile -and !(Test-Path -LiteralPath $BlendFile)) { throw 'File blend non trovato.' }
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi le altre istanze Blender.' }
$previous = $env:BLENDER_MULTIRES_DETAIL_TRANSPORT
try {
    $env:BLENDER_MULTIRES_DETAIL_TRANSPORT = $Mode
    Write-Host "Multires detail transport: $Mode"
    if ($BlendFile) { & $BlenderExe $BlendFile }
    else { & $BlenderExe }
} finally {
    $env:BLENDER_MULTIRES_DETAIL_TRANSPORT = $previous
}
