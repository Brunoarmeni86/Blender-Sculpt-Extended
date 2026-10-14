param(
    [Parameter(Mandatory=$true)][string]$RunDir,
    [string]$SourceDir = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [ValidateRange(30, 3600)][int]$SmokeTimeout = 180
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di completare la portable.' }
$RunDir = [IO.Path]::GetFullPath($RunDir).TrimEnd('\', '/')
if (!(Test-Path -LiteralPath $RunDir -PathType Container)) { throw "Cartella risultati mancante: $RunDir" }
$release = Join-Path $BuildDir 'bin\Release'
$python = Join-Path $release '5.2\python\bin\python.exe'
if (!(Test-Path -LiteralPath $python)) {
    $python = Get-ChildItem -LiteralPath $release -Directory | ForEach-Object {
        $candidate = Join-Path $_.FullName 'python\bin\python.exe'
        if (Test-Path -LiteralPath $candidate) { $candidate }
    } | Select-Object -First 1
}
if (!$python -or !(Test-Path -LiteralPath $python)) { throw "Python della Release non trovato: $release" }
# No INSTALL and no binary replacement: the bundled Python can run in place.
$output = Split-Path -Parent $RunDir
Write-Host "Riprendo copia, test e ZIP: $RunDir"
& $python -S -u (Join-Path $PSScriptRoot 'compila_e_zip.py') --source $SourceDir --build $BuildDir --output $output --resume-run $RunDir --smoke-timeout $SmokeTimeout
if ($LASTEXITCODE) { throw "Completamento fallito. Consulta $RunDir\operation-error.log e i log dei test; backup e binari conservati." }
