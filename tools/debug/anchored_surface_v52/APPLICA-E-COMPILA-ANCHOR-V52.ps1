param(
  [string]$SourceDir = 'C:\BlenderDev\blender',
  [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_conform_test',
  [switch]$ApplyOnly,
  [switch]$Run,
  [switch]$Legacy,
  [switch]$V51,
  [ValidateSet(0,1,2)][int]$DiagnosticLevel = 0,
  [ValidateRange(1,64)][int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
if ($Legacy -and $V51) { throw 'Usa -Legacy oppure -V51, non entrambi.' }
$source = [IO.Path]::GetFullPath($SourceDir).TrimEnd('\','/')
$build = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\','/')
$stable = [IO.Path]::GetFullPath('C:\BlenderDev\build_windows_x64_shareable').TrimEnd('\','/')
$branch = 'feature/anchored-surface-projection-v52'
if ($build -ieq $stable -or $build.StartsWith($stable + '\',[StringComparison]::OrdinalIgnoreCase)) {
  throw 'La build stabile non puo essere usata.'
}
Get-Command git.exe -ErrorAction Stop | Out-Null
if (!(Test-Path -LiteralPath $source)) { throw "Sorgenti mancanti: $source" }
$files = @('source/blender/editors/sculpt_paint/mesh/sculpt.cc',
           'source/blender/editors/sculpt_paint/mesh/sculpt_intern.hh')
# Native patch probes run silently, without PowerShell stderr becoming an exception.
# Async drains avoid pipe deadlocks; actual build errors remain visible.
function Test-AnchorPatch {
  param([string]$Path, [switch]$Reverse)
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = (Get-Command git.exe -ErrorAction Stop).Source
  $mode = if ($Reverse) { '--reverse --check' } else { '--check' }
  $info.Arguments = '-C "{0}" apply {1} -- "{2}"' -f $source, $mode, $Path
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.CreateNoWindow = $true
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $info
  try {
    $null = $process.Start()
    $outTask = $process.StandardOutput.ReadToEndAsync()
    $errTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $null = $outTask.GetAwaiter().GetResult()
    $null = $errTask.GetAwaiter().GetResult()
    return ($process.ExitCode -eq 0)
  }
  finally { $process.Dispose() }
}

$selected = $null
$already = $false
# File-based git apply also supports an extracted source tree without .git.
foreach ($name in @('ANCHOR-V52-FROM-V51.patch','ANCHOR-V52-FROM-V5.patch','ANCHOR-V52-FROM-V41.patch','ANCHOR-V52-FROM-V4.patch')) {
  $candidate = Join-Path $PSScriptRoot $name
  if (!(Test-Path -LiteralPath $candidate)) { throw "Patch mancante: $candidate" }
  if (Test-AnchorPatch -Path $candidate) { $selected = $candidate; break }
  if (Test-AnchorPatch -Path $candidate -Reverse) { $selected = $candidate; $already = $true; break }
}
if (!$selected) { throw 'Nessuna patch applicabile. Sorgenti diversi o conflitto: nessun file modificato.' }
if (Test-Path -LiteralPath (Join-Path $source '.git')) {
  $current = (& git.exe -C $source branch --show-current).Trim()
  if (!$current) { throw 'Checkout detached: seleziona il tuo ramo di sviluppo prima di applicare.' }
  if ($current -ne $branch) {
    & git.exe -C $source show-ref --verify --quiet ('refs/heads/' + $branch)
    if ($LASTEXITCODE -eq 0) { throw "Il ramo $branch esiste gia. Selezionalo e riesegui lo script." }
    & git.exe -C $source switch -c $branch
    if ($LASTEXITCODE -ne 0) { throw 'Creazione del ramo locale fallita.' }
  }
}
else { Write-Host 'Sorgenti estratti senza .git: applicazione locale ai due file, senza inizializzare un repository.' }
if ($already) { Write-Host 'V5.2 gia presente: nessuna patch riapplicata.' }
else {
  $backup = Join-Path (Split-Path $source -Parent) ('Anchor-V52-Backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
  New-Item -ItemType Directory -Path $backup -ErrorAction Stop | Out-Null
  foreach ($relative in $files) {
    $target = Join-Path $backup $relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $target
  }
  & git.exe -C $source apply -- $selected
  if ($LASTEXITCODE -ne 0) { throw "Applicazione fallita. Backup: $backup" }
  Write-Host "V5.2 applicata. Backup: $backup"
}
if ($ApplyOnly) { return }
Get-Command cmake.exe -ErrorAction Stop | Out-Null
$cache = Join-Path $build 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cache)) { throw "Build di test non configurata: $cache" }
$homeLine = Get-Content -LiteralPath $cache | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' } | Select-Object -First 1
if (!$homeLine) { throw 'CMAKE_HOME_DIRECTORY mancante.' }
$configuredSource = [IO.Path]::GetFullPath(($homeLine -split '=',2)[1]).TrimEnd('\','/')
if ($configuredSource -ine $source) { throw "Build associata a un altro sorgente: $configuredSource" }
# Use the existing VS2022 build configuration, including Ninja if originally configured that way.
& cmake.exe --build $build --config Release --target blender --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Compilazione fallita: leggi il primo errore C++.' }
$exe = Join-Path $build 'bin\Release\blender.exe'
if (!(Test-Path -LiteralPath $exe)) { throw "Eseguibile non trovato: $exe" }
Write-Host "Compilazione completata: $exe"
if ($Run) {
  $previousQuality = $env:BLENDER_ANCHOR_PROJECTION_V52
  $previousMode = $env:BLENDER_ANCHOR_PROJECTION_V5
  $previousDiag = $env:BLENDER_ANCHOR_DIAGNOSTICS
  try {
    $env:BLENDER_ANCHOR_PROJECTION_V5 = if ($Legacy) { '0' } else { '1' }
    $env:BLENDER_ANCHOR_PROJECTION_V52 = if ($V51) { '0' } else { '1' }
    $env:BLENDER_ANCHOR_DIAGNOSTICS = [string]$DiagnosticLevel
    if ($DiagnosticLevel -gt 0) {
      $log = Join-Path $PSScriptRoot ('anchor-v52-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
      Write-Host "Log: $log"
      & $exe | Tee-Object -FilePath $log
    }
    else { & $exe }
    if ($LASTEXITCODE -ne 0) { throw "Blender terminato con codice $LASTEXITCODE." }
  }
  finally {
    $env:BLENDER_ANCHOR_PROJECTION_V52 = $previousQuality
    $env:BLENDER_ANCHOR_PROJECTION_V5 = $previousMode
    $env:BLENDER_ANCHOR_DIAGNOSTICS = $previousDiag
  }
}
