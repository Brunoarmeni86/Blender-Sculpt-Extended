param(
    [string]$BlenderExe = 'C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe',
    [string]$ResultsDir = ''
)
$ErrorActionPreference = 'Stop'
$test = Join-Path $PSScriptRoot 'sculpt_extended_multires_bake_cycles_reconstruction.py'
if (!(Test-Path -LiteralPath $test)) { throw "Comparatore mancante: $test" }
if (!$ResultsDir) {
    $latest = Get-ChildItem -LiteralPath $PSScriptRoot -Directory |
        Where-Object { $_.Name -like 'cycles-test-*' } |
        Sort-Object Name -Descending | Select-Object -First 1
    if (!$latest) { throw 'Nessuna cartella cycles-test trovata. Specifica -ResultsDir.' }
    $ResultsDir = $latest.FullName
}
foreach ($mode in @('previous','corrected')) {
    foreach ($file in @('high-reference.f32','normal-only.f32','paired-object.f32','paired-tangent.f32','render-report.json','reconstruction.blend')) {
        $path = Join-Path (Join-Path $ResultsDir $mode) $file
        if (!(Test-Path -LiteralPath $path)) { throw "Risultato mancante: $path" }
    }
}
Write-Host "Confronto dei render esistenti: $ResultsDir"
& $BlenderExe --background --factory-startup --python-exit-code 1 --python $test -- --compare $ResultsDir
if ($LASTEXITCODE) { throw 'Confronto fallito: vedi il primo errore Python.' }
Write-Host "Report: $(Join-Path $ResultsDir 'cycles-reconstruction-comparison.json')"
