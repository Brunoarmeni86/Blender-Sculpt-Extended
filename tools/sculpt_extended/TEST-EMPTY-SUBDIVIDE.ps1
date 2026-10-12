param([string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe')
$ErrorActionPreference = 'Stop'
$test = Join-Path $PSScriptRoot 'sculpt_extended_native_empty_subdivide.py'
if (!(Test-Path -LiteralPath $test)) { $test = Join-Path $PSScriptRoot '..\..\tests\python\sculpt_extended_native_empty_subdivide.py' }
$old = [Environment]::GetEnvironmentVariable('BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE','Process')
try {
    foreach ($mode in @('0','1')) {
        $env:BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE = $mode
        & $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --output (Join-Path $PSScriptRoot "native-empty-subdivide-$mode.json")
        if ($LASTEXITCODE) { throw "Test Subdivide fallito: mode=$mode, vedi il primo errore." }
    }
    & $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --compare $PSScriptRoot
    if ($LASTEXITCODE) { throw 'Confronto Subdivide fallito: vedi il primo errore.' }
} finally {
    [Environment]::SetEnvironmentVariable('BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE',$old,'Process')
}
