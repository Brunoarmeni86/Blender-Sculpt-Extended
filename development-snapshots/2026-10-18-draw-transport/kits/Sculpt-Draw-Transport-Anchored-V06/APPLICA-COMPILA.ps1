param(
    [string]$SourceRoot = 'C:\BlenderDev\blender',
    [string]$BuildRoot = 'C:\BlenderDev\build_windows_x64_shareable',
    [ValidateRange(1,64)][int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima della patch.' }
$activeBuild = @(Get-CimInstance Win32_Process | Where-Object {
    ($_.Name -in @('cmake.exe','ninja.exe','cl.exe','link.exe')) -or
    ($_.Name -eq 'MSBuild.exe' -and $_.CommandLine -notmatch '(?i)/nodemode:1(?:\s|$)')
})
if ($activeBuild.Count -gt 0) { throw ('Compilazione ancora attiva: ' + (($activeBuild | ForEach-Object { "$($_.Name) PID=$($_.ProcessId)" }) -join ', ')) }
$state = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'PATCH-STATE.json') -Raw | ConvertFrom-Json
$target = Join-Path $SourceRoot $state.file
$header = Join-Path $SourceRoot $state.header
$ops = Join-Path $SourceRoot $state.ops_file
$current = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
if ($current -eq $state.after_sha256) { throw 'V06 gia applicata. Usa COMPILA.ps1.' }
$accepted = @($state.accepted_sources | Where-Object { $_.sculpt_sha256 -eq $current })
if ($accepted.Count -ne 1) { throw 'sculpt.cc diverso dalle basi riconosciute: nessuna modifica applicata.' }
$headerWasPresent = Test-Path -LiteralPath $header
if ($accepted[0].header_sha256) {
    if (!$headerWasPresent -or (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash -ne $accepted[0].header_sha256) { throw 'Header non corrispondente alla versione riconosciuta.' }
} elseif ($headerWasPresent) { throw 'Header inatteso sulla base pulita.' }
if ((Get-FileHash -LiteralPath $ops -Algorithm SHA256).Hash -ne $accepted[0].ops_sha256) { throw 'sculpt_ops.cc modificato dopo la base esportata: nessuna modifica applicata.' }
foreach ($name in @('sculpt.cc','sculpt_ops.cc','sculpt_draw_transport_v01.hh')) {
    $payload = Join-Path $PSScriptRoot "payload\$name"
    $expected = $state.payload_hashes.$name
    if (!$expected -or (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash -ne $expected) { throw "Payload non valido: $name" }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$backup = Join-Path $PSScriptRoot "backup-$stamp"
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath $target -Destination (Join-Path $backup 'sculpt.cc')
Copy-Item -LiteralPath $ops -Destination (Join-Path $backup 'sculpt_ops.cc')
if ($headerWasPresent) { Copy-Item -LiteralPath $header -Destination (Join-Path $backup 'sculpt_draw_transport_v01.hh') }
$exe = Join-Path $BuildRoot 'bin\Release\blender.exe'
if (Test-Path -LiteralPath $exe) { Copy-Item -LiteralPath $exe -Destination (Join-Path $backup 'blender.exe') }
@{
    SourceRoot=$SourceRoot; BuildRoot=$BuildRoot; Backup=$backup
    HeaderWasPresent=$headerWasPresent; BeforeSculptSHA256=$current
    BeforeHeaderSHA256=$accepted[0].header_sha256; BeforeOpsSHA256=$accepted[0].ops_sha256
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'ultima-applicazione.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'payload\sculpt_draw_transport_v01.hh') -Destination $header -Force
if ((Get-FileHash -LiteralPath $ops -Algorithm SHA256).Hash -ne $state.payload_hashes.'sculpt_ops.cc') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'payload\sculpt_ops.cc') -Destination $ops -Force
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'payload\sculpt.cc') -Destination $target -Force
Write-Host "V06 applicata da $($accepted[0].label). Backup: $backup" -ForegroundColor Green
& (Join-Path $PSScriptRoot 'COMPILA.ps1') -BuildRoot $BuildRoot -Parallel $Parallel
