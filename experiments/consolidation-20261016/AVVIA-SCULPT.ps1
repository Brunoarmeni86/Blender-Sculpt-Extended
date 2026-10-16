param([string]$BlendFile = '')
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'blender.exe'
$launcher = Join-Path $PSScriptRoot 'SculptExtended\AVVIA-COMPLETO.ps1'
if (!(Test-Path -LiteralPath $exe)) { throw 'Estrai prima tutto il pacchetto Blender, poi avvia AVVIA-SCULPT.cmd dalla cartella estratta.' }
& $launcher -BlenderExe $exe -BlendFile $BlendFile
