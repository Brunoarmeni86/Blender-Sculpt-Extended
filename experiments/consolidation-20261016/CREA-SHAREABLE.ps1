param(
    [string]$SourceRoot = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [string]$OutputRoot = 'C:\BlenderDev\shareable',
    [switch]$Incremental
)
$ErrorActionPreference = 'Stop'

function Read-CoreSources {
    $spec = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'SORGENTI-ATTESI.json')))
    $rows = @()
    foreach ($item in $spec.Files) {
        $path = Join-Path $SourceRoot ([string]$item.Path)
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Sorgente mancante: $path" }
        $text = [IO.File]::ReadAllText($path).Replace("`r`n", "`n").TrimEnd([char[]]"`r`n") + "`n"
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
        if ($hash -cne $item.NormalizedSHA256) {
            throw "Sorgente diverso dal consolidato: $($item.Path). Nessun sorgente viene sostituito. Invia il messaggio prima di compilare."
        }
        $rows += [ordered]@{ Path = $item.Path; NormalizedSHA256 = $hash }
    }
    return $rows
}

$cache = Join-Path $BuildDir 'CMakeCache.txt'
$lines = [IO.File]::ReadAllLines($cache)
$cmakeEntry = $lines | Where-Object { $_ -match '^CMAKE_COMMAND:INTERNAL=' } | Select-Object -First 1
$homeEntry = $lines | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' } | Select-Object -First 1
$prefixEntry = $lines | Where-Object { $_ -match '^CMAKE_INSTALL_PREFIX:PATH=' } | Select-Object -First 1
if (!$cmakeEntry -or !$homeEntry -or !$prefixEntry) { throw 'Cache CMake incompleta.' }
$cmake = ($cmakeEntry -split '=', 2)[1]
if (!(Test-Path -LiteralPath $cmake -PathType Leaf)) { throw 'CMake indicato dalla build non trovato.' }
$homePath = [IO.Path]::GetFullPath((($homeEntry -split '=', 2)[1]).Replace('/', '\')).TrimEnd('\')
if ($homePath -ine [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\')) { throw 'La build usa un altro SourceRoot.' }
$release = [IO.Path]::GetFullPath((Join-Path $BuildDir 'bin\Release')).TrimEnd('\')
$installPrefix = (($prefixEntry -split '=', 2)[1]).Replace('${BUILD_TYPE}', 'Release').Replace('$<CONFIG>', 'Release')
if ([IO.Path]::GetFullPath($installPrefix).TrimEnd('\') -ine $release) { throw "Install prefix diverso da bin\Release: $installPrefix" }
$coreBefore = @(Read-CoreSources)
$runtimeRoot = Join-Path $PSScriptRoot 'SculptExtended'
$runtimeFiles = @(Get-ChildItem -LiteralPath $runtimeRoot -Recurse -File |
    Where-Object { $_.Extension -ne '.pyc' -and $_.FullName -notmatch '[\\/]__pycache__[\\/]' })
if ($runtimeFiles.Count -ne 8) { throw 'Runtime del pacchetto incompleto o modificato. Riestrai lo ZIP in una cartella nuova.' }
$checkpointFile = Join-Path $PSScriptRoot 'CHECKPOINT.txt'
$checkpoint = if (Test-Path -LiteralPath $checkpointFile) { [IO.File]::ReadAllText($checkpointFile).Trim() } else { 'parent 3aad4278504 plus consolidated source hashes' }

if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima della compilazione.' }
$processes = @(Get-CimInstance Win32_Process)
$orphans = @($processes | Where-Object {
    $p = $_
    $p.Name -eq 'MSBuild.exe' -and $p.CommandLine -match '/nodemode:1' -and
    $p.CommandLine -match '/nodeReuse:true' -and
    !($processes | Where-Object { $_.ProcessId -eq $p.ParentProcessId })
})
$busy = @($processes | Where-Object {
    $_.Name -in @('cmake.exe','MSBuild.exe','cl.exe','link.exe','ninja.exe','nvcc.exe') -and
    $_.ProcessId -notin @($orphans.ProcessId)
})
if ($busy.Count) { throw ('Compilazione gia attiva: ' + (($busy | ForEach-Object { $_.Name + ' PID=' + $_.ProcessId }) -join ', ')) }
foreach ($worker in $orphans) {
    $live = Get-CimInstance Win32_Process -Filter ("ProcessId = " + $worker.ProcessId)
    if (!$live) { continue }
    if ($live.CreationDate -ne $worker.CreationDate -or $live.CommandLine -ne $worker.CommandLine -or
        (Get-Process -Id $live.ParentProcessId -ErrorAction SilentlyContinue)) { throw 'Processi MSBuild cambiati: ripetere il comando.' }
    Stop-Process -Id $worker.ProcessId -ErrorAction SilentlyContinue
    Wait-Process -Id $worker.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
}
if (Get-Process -Name cmake,MSBuild,cl,link,ninja,nvcc -ErrorAction SilentlyContinue) { throw 'Sono ancora presenti processi di compilazione.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$work = Join-Path $PSScriptRoot ('build-' + $stamp)
New-Item -ItemType Directory -Path $work | Out-Null
$buildLog = Join-Path $work 'build.log'
$buildArgs = @('--build', $BuildDir, '--config', 'Release', '--target', 'INSTALL', '--parallel', '8')
if (!$Incremental) { $buildArgs += '--clean-first' }
Write-Host 'Compilazione Release e installazione con la configurazione CMake/GPU esistente.' -ForegroundColor Cyan
& $cmake @buildArgs | Tee-Object -FilePath $buildLog
if ($LASTEXITCODE -ne 0) { throw "Build fallita. Non avviare la vecchia build. Log: $buildLog" }
$null = @(Read-CoreSources)
$exe = Join-Path $release 'blender.exe'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'blender.exe compilato non trovato.' }
$installManifest = Join-Path $BuildDir 'install_manifest.txt'
if (!(Test-Path -LiteralPath $installManifest -PathType Leaf)) { throw 'Manifest di installazione CMake mancante: nessun archivio creato.' }
$installed = @([IO.File]::ReadAllLines($installManifest) | Where-Object { $_.Trim() } | Sort-Object -Unique)
$installFiles = @()
foreach ($entry in $installed) {
    $full = [IO.Path]::GetFullPath($entry.Replace('/', '\'))
    if (!$full.StartsWith($release + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "File installato fuori da Release: $full" }
    if (!(Test-Path -LiteralPath $full -PathType Leaf)) { throw "File installato mancante: $full" }
    $installFiles += [pscustomobject]@{ Source = $full; Relative = $full.Substring($release.Length + 1) }
}
if (!($installFiles | Where-Object { $_.Relative -ieq 'blender.exe' })) { throw 'Il manifest non include blender.exe.' }

$smokeResult = Join-Path $work 'smoke.json'
$smokeLog = Join-Path $work 'smoke.log'
$smokeErrors = Join-Path $work 'smoke-errors.log'
$smokeScript = Join-Path $PSScriptRoot 'VERIFICA-BUILD.py'
$argsSmoke = @('--background', '--factory-startup', '--python-exit-code', '1', '--python', ('"' + $smokeScript + '"'), '--', ('"' + $smokeResult + '"'))
$process = Start-Process -FilePath $exe -ArgumentList $argsSmoke -WorkingDirectory $release -RedirectStandardOutput $smokeLog -RedirectStandardError $smokeErrors -Wait -PassThru
if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $smokeResult)) { throw "Verifica build fallita. Log: $smokeLog ; $smokeErrors" }
$smoke = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($smokeResult))
if (!$smoke.Success) { throw "Funzioni richieste mancanti: $smokeResult" }

$folder = Join-Path $OutputRoot ('Blender-Sculpt-Extended-' + $stamp)
if (Test-Path -LiteralPath $folder) { throw 'Cartella destinazione gia presente.' }
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$count = 0
foreach ($item in $installFiles) {
    $target = Join-Path $folder $item.Relative
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item -LiteralPath $item.Source -Destination $target
    $count++
    if ($count % 500 -eq 0) { Write-Host "File installati copiati: $count / $($installFiles.Count)" }
}
# Install the exact same complete runtime into the current build and the portable.
# Keep a backup of overwritten runtime files; never copy old logs into the portable.
$backup = Join-Path $work 'runtime-backup'
foreach ($file in $runtimeFiles) {
    $relative = $file.FullName.Substring($runtimeRoot.Length + 1)
    $current = Join-Path (Join-Path $release 'SculptExtended') $relative
    if (Test-Path -LiteralPath $current -PathType Leaf) {
        $saved = Join-Path $backup $relative
        New-Item -ItemType Directory -Path (Split-Path $saved) -Force | Out-Null
        Copy-Item -LiteralPath $current -Destination $saved
    }
    foreach ($rootPath in @($release, $folder)) {
        $target = Join-Path (Join-Path $rootPath 'SculptExtended') $relative
        New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
}
foreach ($name in @('AVVIA-SCULPT.ps1','AVVIA-SCULPT.cmd','LEGGIMI-PORTABLE.txt',
                   'RACCOGLI-LOG.ps1','RACCOGLI-LOG.cmd','GUIDA-TEST.txt','SCHEDA-SEGNALAZIONE.txt')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $folder $name)
}
New-Item -ItemType Directory -Path (Join-Path $folder ($smoke.VersionDirectory + '\config')) -Force | Out-Null
$exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath (Join-Path $folder 'blender.exe')).Hash -cne $exeHash) { throw 'Copia eseguibile non corrispondente.' }
$cycles = @($lines | Where-Object { $_ -match '^WITH_CYCLES[^:]*:BOOL=' })
$info = [ordered]@{
    Timestamp = (Get-Date).ToString('o'); Checkpoint = $checkpoint
    Configuration = 'Release'; CleanFirst = !$Incremental; CMakeCachePreserved = $true
    ExecutableSHA256 = $exeHash; CheckedCoreSources = $coreBefore
    SourceCheckScope = 'Eight core files; does not certify every other working-source file against Git.'
    InstalledFileCount = $installFiles.Count; CyclesConfiguration = $cycles
    Runtime = 'Complete validated profile; Strength/Visibility Undo enabled; no verification observer'
    Smoke = $smoke
}
[IO.File]::WriteAllText((Join-Path $folder 'BUILD-INFO.json'), ($info | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = $folder + '.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($folder, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    if (!($archive.Entries | Where-Object { $_.FullName -eq 'blender.exe' }) -or
        !($archive.Entries | Where-Object { ($_.FullName -replace '\\','/') -eq 'SculptExtended/AVVIA-COMPLETO.ps1' })) {
        throw 'Verifica struttura ZIP fallita.'
    }
} finally { $archive.Dispose() }
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
[IO.File]::WriteAllText(($zip + '.sha256.txt'), ($zipHash + '  ' + [IO.Path]::GetFileName($zip) + "`r`n"))
Write-Host "SHAREABLE: $zip" -ForegroundColor Green
Write-Host "CARTELLA: $folder"
Write-Host "LOG BUILD: $buildLog"
Write-Host "SHA256: $zipHash"
Write-Host 'Avvio completo della copia condivisibile: AVVIA-SCULPT.cmd'
