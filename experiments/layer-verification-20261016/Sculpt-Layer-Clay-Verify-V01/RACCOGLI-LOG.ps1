$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di raccogliere i log.' }
$meta = Join-Path $PSScriptRoot 'ultima-prova.json'
if (!(Test-Path -LiteralPath $meta)) { throw 'Avvia prima la prova con AVVIA.ps1 di questo pacchetto.' }
$session = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($meta))
$start = [DateTimeOffset]::Parse($session.StartTime).LocalDateTime
$logs = @(Get-ChildItem -LiteralPath $session.LogRoot -Filter 'sculpt-polypaint-next-*.log' -File |
    Where-Object { $_.LastWriteTime -ge $start } | Sort-Object LastWriteTime)
if (!$logs.Count) { throw 'Nessun log della prova trovato.' }
$hasAudit = $false
foreach ($file in $logs) {
    if (Select-String -LiteralPath $file.FullName -Pattern 'CLAY(_STRIPS)?_[0-9]+_(EMPTY|FILLED)_' -Quiet) { $hasAudit = $true }
}
if (!$hasAudit) { throw 'Mancano le impronte. Apri la prova e usa N > Multires > Verifica Clay.' }
$folder = Join-Path $PSScriptRoot ('risultati-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $folder | Out-Null
Copy-Item -LiteralPath $meta -Destination $folder
foreach ($file in $logs) { Copy-Item -LiteralPath $file.FullName -Destination $folder }
$zip = $folder + '.zip'
Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zip
Write-Host "RISULTATI: $zip" -ForegroundColor Green
