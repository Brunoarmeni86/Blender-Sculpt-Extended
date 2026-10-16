$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di raccogliere i log.' }
$root = Join-Path $PSScriptRoot 'SculptExtended\experiment-tools'
$normal = Get-ChildItem -LiteralPath $root -Filter 'sculpt-polypaint-next-*.log' -File |
    Where-Object { $_.BaseName -notmatch '-errors$' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$normal) { throw 'Nessun log trovato: avvia Blender con AVVIA-SCULPT.cmd e riproduci il problema.' }
$folder = Join-Path $PSScriptRoot ('segnalazione-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $folder | Out-Null
Copy-Item -LiteralPath $normal.FullName -Destination $folder
$errors = Join-Path $root ($normal.BaseName + '-errors.log')
if (Test-Path -LiteralPath $errors) { Copy-Item -LiteralPath $errors -Destination $folder }
foreach ($name in @('BUILD-INFO.json', 'SCHEDA-SEGNALAZIONE.txt')) {
    $file = Join-Path $PSScriptRoot $name
    if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination $folder }
}
$zip = $folder + '.zip'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($folder, $zip)
Write-Host "SEGNALAZIONE: $zip" -ForegroundColor Green
Write-Host 'Invia questo ZIP al referente dei test. Nessun file .blend o configurazione utente e stato allegato.'
