param([string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe')
$ErrorActionPreference = 'Stop'
$test = Join-Path $PSScriptRoot 'sculpt_extended_multires_bake_native_coverage.py'
if (!(Test-Path -LiteralPath $test)) {
    $test = Join-Path $PSScriptRoot '..\..\tests\python\sculpt_extended_multires_bake_native_coverage.py'
}
if (!(Test-Path -LiteralPath $test)) { throw "Test mancante: $test" }
foreach ($coverage in @('1', '0')) {
    $env:BLENDER_MULTIRES_BAKE_SS = '4'
    $env:BLENDER_MULTIRES_BAKE_UV_COVERAGE = $coverage
    $report = Join-Path $PSScriptRoot "native-coverage-$coverage.json"
    & $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --output $report
    if ($LASTEXITCODE) { throw "Test nativo fallito: copertura=$coverage. Vedi il primo errore Python/Blender." }
}
$env:BLENDER_MULTIRES_BAKE_UV_COVERAGE = '1'
Write-Host 'Confronto nativo ON/OFF completato: i due report JSON sono nella cartella del test.'
