param(
    [ValidateSet("Baseline","NoSweep","NoAutoSmooth","NoGravity","NoProxy","Exposure","FreshDirection","Mirror","Combined")]
    [string]$Profile = "NoAutoSmooth",
    [string]$Root = "C:\BlenderDev"
)
$ErrorActionPreference = "Stop"
$previous = Get-ChildItem "$Root\V31D3-Diagnostic-*.log" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $previous) { throw "Original V31-D3 log not found" }
$settings = @(Get-Content $previous.FullName | Where-Object { $_ -match '^BLENDER_SCULPT_[A-Z0-9_]+=' })
if ($settings.Count -eq 0) { throw "Original settings not found" }
Get-ChildItem Env:BLENDER_SCULPT* | ForEach-Object { Remove-Item ("Env:" + $_.Name) }
foreach ($line in $settings) {
    $pair = $line -split '=',2
    [Environment]::SetEnvironmentVariable($pair[0],$pair[1],"Process")
}
$env:BLENDER_SCULPT_REACH_V31D4="0"
$env:BLENDER_SCULPT_DIAG_V31D5="1"
$env:BLENDER_SCULPT_PROFILE_V26="1"
$env:BLENDER_SCULPT_EXPOSURE_V31D5="0"
$env:BLENDER_SCULPT_FRESH_DIRECTION_V31D5="0"
$env:BLENDER_SCULPT_MIRROR_V31D5="0"
$env:BLENDER_SCULPT_NO_AUTOSMOOTH_V31D5="0"
$env:BLENDER_SCULPT_NO_GRAVITY_V31D5="0"
switch ($Profile) {
    "NoSweep" {$env:BLENDER_SCULPT_SWEEP_V29="0"}
    "NoAutoSmooth" {$env:BLENDER_SCULPT_NO_AUTOSMOOTH_V31D5="1"}
    "NoGravity" {$env:BLENDER_SCULPT_NO_GRAVITY_V31D5="1"}
    "NoProxy" {$env:BLENDER_SCULPT_PROXY_V31D3="0"}
    "Exposure" {
        $env:BLENDER_SCULPT_EXPOSURE_V31D5="1"
        $env:BLENDER_SCULPT_V31D5_EXPOSURE_BONUS="0.20"
    }
    "FreshDirection" {$env:BLENDER_SCULPT_FRESH_DIRECTION_V31D5="1"}
    "Mirror" {
        $env:BLENDER_SCULPT_MIRROR_V31D5="1"
        $env:BLENDER_SCULPT_NO_AUTOSMOOTH_V31D5="1"
        $env:BLENDER_SCULPT_NO_GRAVITY_V31D5="1"
    }
    "Combined" {
        $env:BLENDER_SCULPT_SWEEP_V29="0"
        $env:BLENDER_SCULPT_NO_AUTOSMOOTH_V31D5="1"
        $env:BLENDER_SCULPT_NO_GRAVITY_V31D5="1"
        $env:BLENDER_SCULPT_EXPOSURE_V31D5="1"
        $env:BLENDER_SCULPT_V31D5_EXPOSURE_BONUS="0.20"
        $env:BLENDER_SCULPT_FRESH_DIRECTION_V31D5="1"
    }
}
$exe = "$Root\install-physics-v16\blender.exe"
if (-not (Test-Path $exe)) { throw "Blender executable not found" }
$log = "$Root\V31D5-$Profile-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
& $exe --version | Out-File $log
Get-ChildItem Env:BLENDER_SCULPT* | Sort-Object Name |
    ForEach-Object {"$($_.Name)=$($_.Value)"} | Out-File $log -Append
Write-Host "V31D5 $Profile - $log"
$ErrorActionPreference="Continue"
cmd.exe /d /c "`"$exe`" 2>&1" | Tee-Object -FilePath $log -Append
Write-Host "Exit code: $LASTEXITCODE"
