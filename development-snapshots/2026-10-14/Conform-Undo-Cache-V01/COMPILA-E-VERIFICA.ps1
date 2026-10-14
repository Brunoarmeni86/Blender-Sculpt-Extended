param(
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [string]$OutputDir = 'C:\BlenderDev\Experimental-Builds',
    [ValidateRange(1, 256)][int]$Jobs = 4,
    [ValidateRange(30, 3600)][int]$SmokeTimeout = 300,
    [string]$RunDir = '',
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di compilare.' }
$release = Join-Path $BuildDir 'bin\Release'
$python = Join-Path $release '5.2\python\bin\python.exe'
if (!(Test-Path -LiteralPath $python)) {
    $python = Get-ChildItem -LiteralPath $release -Directory | ForEach-Object {
        $candidate = Join-Path $_.FullName 'python\bin\python.exe'
        if (Test-Path -LiteralPath $candidate) { $candidate }
    } | Select-Object -First 1
}
if (!$python -or !(Test-Path -LiteralPath $python)) { throw "Python incluso nella Release non trovato: $release" }
# Run outside bin/Release: INSTALL may replace the bundled Python executable.
# A process running from that directory can otherwise lock its own install target.
$pythonRoot = Split-Path -Parent (Split-Path -Parent $python)
$runnerDir = Join-Path $OutputDir ('Runner-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$stagedRoot = Join-Path $runnerDir 'python'
New-Item -ItemType Directory -Path $stagedRoot -Force | Out-Null
$pythonFiles = @(Get-ChildItem -LiteralPath $pythonRoot -Recurse -File)
Write-Host "Preparo Python esterno alla build: $runnerDir; $($pythonFiles.Count) file"
$copied = 0
$lastProgress = Get-Date
foreach ($item in $pythonFiles) {
    $relative = $item.FullName.Substring($pythonRoot.Length).TrimStart('\', '/')
    $target = Join-Path $stagedRoot $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $item.FullName -Destination $target
    $copied++
    if (((Get-Date) - $lastProgress).TotalSeconds -ge 5) {
        Write-Host "Python: $copied/$($pythonFiles.Count) file"
        $lastProgress = Get-Date
    }
}
$python = Join-Path $stagedRoot 'bin\python.exe'
Write-Host 'Compilazione incrementale della Conform/Undo Cache: backup e log con avanzamento.'
$runnerArgs = @('--source', $SourceDir, '--build', $BuildDir, '--output', $OutputDir, '--jobs', [string]$Jobs, '--smoke-timeout', [string]$SmokeTimeout)
if ($RunDir) { $runnerArgs += @('--resume-run', $RunDir) }
if ($Restore) { $runnerArgs += '--restore' }
& $python -S -u (Join-Path $PSScriptRoot 'runner.py') @runnerArgs
if ($LASTEXITCODE) { throw 'Conform/Undo Cache non completata. Consulta operation-error.log nella cartella stampata; backup conservati.' }
