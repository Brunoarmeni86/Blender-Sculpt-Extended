param(
  [string]$SourceDir = 'C:\BlenderDev\blender',
  [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_conform_test',
  [switch]$ApplyOnly,
  [switch]$RunDiagnostics,
  [ValidateSet(1,2)][int]$DiagnosticLevel = 1,
  [ValidateRange(1,64)][int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$branch = 'feature/anchored-surface-diagnostics-v41'
$patch = Join-Path $PSScriptRoot 'anchored-v41-diagnostics.patch'
$source = [IO.Path]::GetFullPath($SourceDir).TrimEnd('\','/')
$build = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\','/')
$stable = [IO.Path]::GetFullPath('C:\BlenderDev\build_windows_x64_shareable').TrimEnd('\','/')
if ($build -ieq $stable -or $build.StartsWith($stable + '\',[StringComparison]::OrdinalIgnoreCase)) {
  throw 'La directory stabile non puo essere usata per compilare.'
}
if (!(Test-Path -LiteralPath $patch)) { throw "Patch mancante: $patch" }
Get-Command git.exe -ErrorAction Stop | Out-Null
& git.exe -C $source rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0) { throw 'SourceDir deve essere il checkout Git del progetto.' }
$files = @('source/blender/editors/sculpt_paint/mesh/sculpt.cc',
           'source/blender/editors/sculpt_paint/mesh/sculpt_intern.hh')
# Check applicability before switching branches or writing files. No reset, checkout of files or pull.
& git.exe -C $source apply --check -- $patch
$applicable = $LASTEXITCODE -eq 0
if (!$applicable) {
  & git.exe -C $source apply --reverse --check -- $patch
  if ($LASTEXITCODE -ne 0) { throw 'Sorgenti diversi o conflitto: nessun file modificato.' }
  Write-Host 'Diagnostica gia applicata.'
}
$current = (& git.exe -C $source branch --show-current).Trim()
if ($LASTEXITCODE -ne 0 -or !$current) { throw 'Checkout detached: seleziona il tuo ramo V4 prima di applicare.' }
if ($current -ne $branch) {
  & git.exe -C $source show-ref --verify --quiet ('refs/heads/' + $branch)
  if ($LASTEXITCODE -eq 0) {
    throw "Il ramo $branch esiste gia. Selezionalo e riesegui lo script; nessuno switch automatico su un ramo esistente."
  }
  & git.exe -C $source switch -c $branch
  if ($LASTEXITCODE -ne 0) { throw 'Creazione del ramo locale fallita.' }
}
if ($applicable) {
  $backup = Join-Path (Split-Path $source -Parent) ('Anchor-V41-Backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
  New-Item -ItemType Directory -Path $backup -ErrorAction Stop | Out-Null
  foreach ($relative in $files) {
    $target = Join-Path $backup $relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $target
  }
  & git.exe -C $source rev-parse HEAD | Set-Content -LiteralPath (Join-Path $backup 'parent-commit.txt')
  & git.exe -C $source apply -- $patch
  if ($LASTEXITCODE -ne 0) { throw "Applicazione fallita. Backup: $backup" }
  Write-Host "Patch applicata. Backup: $backup"
}
if ($ApplyOnly) { return }
Get-Command cmake.exe -ErrorAction Stop | Out-Null
$cache = Join-Path $build 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cache)) { throw "Build di test non configurata: $cache" }
$cacheLines = Get-Content -LiteralPath $cache
$homeLine = $cacheLines | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' } | Select-Object -First 1
if (!$homeLine) { throw 'CMAKE_HOME_DIRECTORY mancante.' }
$configuredSource = [IO.Path]::GetFullPath(($homeLine -split '=',2)[1]).TrimEnd('\','/')
if ($configuredSource -ine $source) { throw "Build associata a un altro sorgente: $configuredSource" }
if (!($cacheLines -contains 'CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022')) {
  throw 'La build indicata non usa Visual Studio 2022.'
}
& cmake.exe --build $build --config Release --target blender --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Compilazione fallita: leggi il primo errore C++.' }
$exe = Join-Path $build 'bin\Release\blender.exe'
if (!(Test-Path -LiteralPath $exe)) { throw "Eseguibile non trovato: $exe" }
Write-Host "Compilazione completata: $exe"
if ($RunDiagnostics) {
  $previous = $env:BLENDER_ANCHOR_DIAGNOSTICS
  $log = Join-Path $PSScriptRoot ('anchor-v41-level' + $DiagnosticLevel + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
  try {
    $env:BLENDER_ANCHOR_DIAGNOSTICS = [string]$DiagnosticLevel
    Write-Host "Diagnostica $DiagnosticLevel. Chiudi Blender per terminare. Log: $log"
    & $exe | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) { throw "Blender terminato con codice $LASTEXITCODE. Log: $log" }
  }
  finally { $env:BLENDER_ANCHOR_DIAGNOSTICS = $previous }
}
