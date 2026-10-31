param([string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe')
$ErrorActionPreference = 'Stop'
$test = Join-Path $PSScriptRoot 'sculpt_extended_multires_bake_cycles_reconstruction.py'
if (!(Test-Path -LiteralPath $test)) { $test = Join-Path $PSScriptRoot '..\..\tests\python\sculpt_extended_multires_bake_cycles_reconstruction.py' }
if (!(Test-Path -LiteralPath $test)) { throw "Test mancante: $test" }
if (!(Test-Path -LiteralPath $BlenderExe)) { throw "Blender mancante: $BlenderExe" }
$outputDir = Join-Path $PSScriptRoot ('cycles-test-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $outputDir | Out-Null
$names = @('BLENDER_MULTIRES_BAKE_SS', 'BLENDER_MULTIRES_BAKE_UV_COVERAGE', 'BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:BLENDER_MULTIRES_BAKE_SS = '4'
    $env:BLENDER_MULTIRES_BAKE_UV_COVERAGE = '1'
    foreach ($mode in @('previous','corrected')) {
        $env:BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN = '0'
        if ($mode -eq 'corrected') { $env:BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN = '1' }
        $modeDir = Join-Path $outputDir $mode
        & $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --output $modeDir
        if ($LASTEXITCODE) { throw "Test Cycles fallito: $mode. Vedi il primo errore Blender/Python." }
    }
    & $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --compare $outputDir
    if ($LASTEXITCODE) { throw 'Confronto Cycles fallito: vedi il primo errore.' }
    Get-FileHash -Algorithm SHA256 -LiteralPath $BlenderExe | Format-List | Out-File (Join-Path $outputDir 'binary-sha256.txt')
    Write-Host "Report e immagini: $outputDir"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
