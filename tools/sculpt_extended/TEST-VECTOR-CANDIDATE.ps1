param([string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe')
$ErrorActionPreference = 'Stop'
$test = Join-Path $PSScriptRoot 'sculpt_extended_multires_bake_native_vector_frame.py'
if (!(Test-Path -LiteralPath $test)) { $test = Join-Path $PSScriptRoot '..\..\tests\python\sculpt_extended_multires_bake_native_vector_frame.py' }
if (!(Test-Path -LiteralPath $test)) { throw "Script mancante: $test" }
if (!(Test-Path -LiteralPath $BlenderExe)) { throw "Blender mancante: $BlenderExe" }
$names = @('BLENDER_MULTIRES_BAKE_SS','BLENDER_MULTIRES_BAKE_UV_COVERAGE','BLENDER_MULTIRES_BAKE_VECTOR_CYCLES_TBN')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
try {
    $env:BLENDER_MULTIRES_BAKE_SS = '4'
    $env:BLENDER_MULTIRES_BAKE_UV_COVERAGE = '1'
    $env:BLENDER_MULTIRES_BAKE_VECTOR_CYCLES_TBN = '1'
    $report = Join-Path $PSScriptRoot 'native-vector-frame-candidate.json'
    & $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --output $report
    if ($LASTEXITCODE) { throw 'Audit Vector fallito: vedi il primo errore Python/Blender.' }
    Get-FileHash -Algorithm SHA256 -LiteralPath $BlenderExe | Format-List | Out-File (Join-Path $PSScriptRoot 'binary-sha256.txt')
    Write-Host "Report: $report"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
