$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima del ripristino.' }
$activeBuild = @(Get-CimInstance Win32_Process | Where-Object {
    ($_.Name -in @('cmake.exe','ninja.exe','cl.exe','link.exe')) -or
    ($_.Name -eq 'MSBuild.exe' -and $_.CommandLine -notmatch '(?i)/nodemode:1(?:\s|$)')
})
if ($activeBuild.Count -gt 0) { throw ('Compilazione ancora attiva: ' + (($activeBuild | ForEach-Object { "$($_.Name) PID=$($_.ProcessId)" }) -join ', ')) }
$info = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ultima-applicazione.json') -Raw | ConvertFrom-Json
$state = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'PATCH-STATE.json') -Raw | ConvertFrom-Json
$target = Join-Path $info.SourceRoot $state.file
$header = Join-Path $info.SourceRoot $state.header
$ops = Join-Path $info.SourceRoot $state.ops_file
foreach ($pair in @(@($target,'sculpt.cc'),@($header,'sculpt_draw_transport_v01.hh'),@($ops,'sculpt_ops.cc'))) {
    if (!(Test-Path -LiteralPath $pair[0]) -or (Get-FileHash -LiteralPath $pair[0] -Algorithm SHA256).Hash -ne $state.payload_hashes.($pair[1])) { throw "Sorgente modificato dopo V07: $($pair[1]). Ripristino interrotto." }
}
$original = Join-Path $info.Backup 'sculpt.cc'
$originalOps = Join-Path $info.Backup 'sculpt_ops.cc'
if ((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -ne $info.BeforeSculptSHA256) { throw 'Backup sculpt.cc non corrispondente.' }
if ((Get-FileHash -LiteralPath $originalOps -Algorithm SHA256).Hash -ne $info.BeforeOpsSHA256) { throw 'Backup sculpt_ops.cc non corrispondente.' }
$originalHeader = Join-Path $info.Backup 'sculpt_draw_transport_v01.hh'
if ($info.HeaderWasPresent -and (!(Test-Path -LiteralPath $originalHeader) -or (Get-FileHash -LiteralPath $originalHeader -Algorithm SHA256).Hash -ne $info.BeforeHeaderSHA256)) { throw 'Backup header non corrispondente.' }
Copy-Item -LiteralPath $original -Destination $target -Force
Copy-Item -LiteralPath $originalOps -Destination $ops -Force
if ($info.HeaderWasPresent) { Copy-Item -LiteralPath $originalHeader -Destination $header -Force }
else { Remove-Item -LiteralPath $header }
$oldExe = Join-Path $info.Backup 'blender.exe'
if (Test-Path -LiteralPath $oldExe) { Copy-Item -LiteralPath $oldExe -Destination (Join-Path $info.BuildRoot 'bin\Release\blender.exe') -Force }
Write-Host 'Stato precedente alla V07 ripristinato.' -ForegroundColor Green
