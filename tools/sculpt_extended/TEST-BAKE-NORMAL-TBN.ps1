param([string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe')
$ErrorActionPreference = 'Stop'
$test = Join-Path $PSScriptRoot 'sculpt_extended_multires_bake_native_normal_tbn.py'
if (!(Test-Path -LiteralPath $test)) {
    $test = Join-Path $PSScriptRoot '..\..\tests\python\sculpt_extended_multires_bake_native_normal_tbn.py'
}
if (!(Test-Path -LiteralPath $test)) { throw "Test mancante: $test" }
if (!(Test-Path -LiteralPath $BlenderExe)) { throw "Blender mancante: $BlenderExe" }
$names = @('BLENDER_MULTIRES_BAKE_SS', 'BLENDER_MULTIRES_BAKE_UV_COVERAGE', 'BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:BLENDER_MULTIRES_BAKE_UV_COVERAGE = '1'
    foreach ($mode in @('reference', 'previous', 'corrected')) {
        $env:BLENDER_MULTIRES_BAKE_SS = '4'
        $env:BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN = '1'
        if ($mode -eq 'reference') { $env:BLENDER_MULTIRES_BAKE_SS = '1' }
        if ($mode -eq 'previous') { $env:BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN = '0' }
        $report = Join-Path $PSScriptRoot "native-normal-$mode.json"
        & $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --output $report
        if ($LASTEXITCODE) { throw "Test fallito: $mode. Vedi il primo errore Python/Blender." }
    }
    & $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --compare $PSScriptRoot
    if ($LASTEXITCODE) { throw 'Confronto fallito: vedi il primo errore Python.' }
    Get-FileHash -Algorithm SHA256 -LiteralPath $BlenderExe | Format-List | Out-File (Join-Path $PSScriptRoot 'native-normal-binary.txt')
    Write-Host 'Report: native-normal-comparison.json. Il confronto con 1x misura una differenza, non certifica un render Cycles.'
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
