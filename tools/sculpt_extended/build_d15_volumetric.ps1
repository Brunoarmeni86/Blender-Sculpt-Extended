# D15 clean Windows build. Keep the previous install and asset libraries intact.
[CmdletBinding()]
param(
    [string]$Source = "C:\BlenderDev\blender-physics-v16",
    [string]$BuildDirectory = "C:\BlenderDev\build-physics-v16",
    [string]$InstallDirectory = "C:\BlenderDev\install-physics-d15-volumetric",
    [string]$PreviousInstall = "C:\BlenderDev\install-physics-d13-recovery",
    [string]$AssetDirectory = "C:\BlenderDev\assets-d15-native",
    [string]$PtxDirectory = "C:\BlenderDev\warp-ptx-d15-volumetric",
    [string]$WarpRoot = "C:\BlenderDev\third_party\warp-1.19.0",
    [string]$Python = "C:\BlenderDev\blender\lib\windows_x64\python\313\bin\python.exe"
)
$ErrorActionPreference = "Stop"
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$gpu = Join-Path $Source "experimental\sculpt_physics_gpu"
$cache = Join-Path $BuildDirectory "CMakeCache.txt"
foreach ($path in @($cmake,$Python,$cache,(Join-Path $WarpRoot "warp\bin\warp.dll"))) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required path missing: $path" }
}
if (!(Select-String -Path $cache -Pattern "^CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022$" -Quiet)) {
    throw "Existing build must use Visual Studio 2022 (v143)."
}
if (Test-Path -LiteralPath $AssetDirectory) {
    if (Get-ChildItem -LiteralPath $AssetDirectory -Force | Select-Object -First 1) {
        throw "Asset directory is not empty. Choose a new -AssetDirectory."
    }
}
Set-Location $Source
if ((git branch --show-current) -ne "experiment/physics-d15-volumetric-only") {
    throw "Switch to experiment/physics-d15-volumetric-only first."
}
if (git status --porcelain) { throw "Local source changes present; preserve them before building." }
git log -1 --oneline

New-Item -ItemType Directory -Force -Path $PtxDirectory | Out-Null
$env:PYTHONPATH = $WarpRoot
$required = @{
    "blender_xpbd_jacobi.sm75.ptx" = @("jacobi_shape_step_2135c77f_cuda_kernel_forward")
    "blender_smooth_a3.sm75.ptx" = @("sculpt_smooth_laplacian_b88c9770_cuda_kernel_forward","sculpt_smooth_scatter_4021f8a1_cuda_kernel_forward")
}
function Test-PtxEntries([string]$File, [string[]]$Names) {
    if (!(Test-Path -LiteralPath $File)) { return $false }
    $raw = Get-Content -LiteralPath $File -Raw
    foreach ($name in $Names) {
        if ($raw -notmatch ("\.entry\s+" + [regex]::Escape($name) + "\s*\(")) {
            return $false
        }
    }
    return $true
}
$regenerate = $false
foreach ($name in $required.Keys) {
    $old = Join-Path $PreviousInstall $name
    $staged = Join-Path $PtxDirectory $name
    if (Test-PtxEntries $old $required[$name]) {
        if ([IO.Path]::GetFullPath($old) -ne [IO.Path]::GetFullPath($staged)) {
            Copy-Item -LiteralPath $old -Destination $staged -Force
        }
    }
    if (!(Test-PtxEntries $staged $required[$name])) { $regenerate = $true }
}
if ($regenerate) {
    & $Python (Join-Path $gpu "export_native_warp_ptx.py") --output $PtxDirectory
    if ($LASTEXITCODE -ne 0) { throw "Existing Warp PTX export failed." }
}
foreach ($name in $required.Keys) {
    if (!(Test-PtxEntries (Join-Path $PtxDirectory $name) $required[$name])) {
        throw "Native PTX symbol mismatch: $name"
    }
}

# Always regenerate only the new independent module from this checkout.
& $Python (Join-Path $gpu "prototype_warp_smooth_multiscale.py") --output $PtxDirectory
if ($LASTEXITCODE -ne 0) { throw "Multiscale CUDA export/parity test failed." }
$multiPtx = Join-Path $PtxDirectory "blender_smooth_multiscale.sm75.ptx"
$multiSymbols = Join-Path $PtxDirectory "blender_smooth_multiscale.sm75.symbols"
$symbol = (Get-Content -LiteralPath $multiSymbols -Raw).Trim()
if ($symbol -notmatch "^sculpt_smooth_multiscale_\w+_cuda_kernel_forward$" -or
    !(Test-PtxEntries $multiPtx @($symbol))) { throw "Invalid multiscale manifest." }

& $cmake -S $Source -B $BuildDirectory `
    "-DCMAKE_INSTALL_PREFIX=$InstallDirectory" `
    "-DSCULPT_WARP_ROOT=$WarpRoot" `
    "-DSCULPT_WARP_PTX_DIR=$PtxDirectory" `
    "-DWITH_SCULPT_XPBD_GRAB_EXPERIMENTAL=ON" `
    "-DWITH_SCULPT_XPBD_GRAB_EXECUTOR_V14=OFF" `
    "-DWITH_SCULPT_XPBD_OTHER_BRUSHES_V15=OFF" `
    "-DWITH_BSE_VOLUMETRIC_ONLY=ON"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
& $cmake --build $BuildDirectory --config Release --target blender -- /m:1
if ($LASTEXITCODE -ne 0) { throw "Blender compilation failed." }
& $cmake --build $BuildDirectory --config Release --target install -- /m:1
if ($LASTEXITCODE -ne 0) { throw "Blender installation failed." }

foreach ($name in @("warp.dll","blender_xpbd_jacobi.sm75.ptx","blender_smooth_a3.sm75.ptx",
    "blender_smooth_multiscale.sm75.ptx","blender_smooth_multiscale.sm75.symbols")) {
    if (!(Test-Path -LiteralPath (Join-Path $InstallDirectory $name))) {
        throw "Install packaging missing $name"
    }
    Write-Host "[OK] $name"
}
$exe = Join-Path $InstallDirectory "blender.exe"
& $exe --version
if ($LASTEXITCODE -ne 0) { throw "Blender version smoke failed." }
& $exe --background --factory-startup --python-exit-code 1 `
    --python (Join-Path $Source "experimental\sculpt_physics_cpu\create_physical_brush_assets.py") `
    -- --output $AssetDirectory
if ($LASTEXITCODE -ne 0) { throw "D15 native asset generation failed." }
Write-Host "[D15 volumetric] Install: $InstallDirectory"
Write-Host "[D15 volumetric] Assets:  $AssetDirectory"
Write-Host "For NVIDIA Warp Smooth > Very Strong, set BLENDER_SCULPT_D13_SMOOTH_RECOVERY to classic, original, extended or adaptive BEFORE Blender starts. Enable BLENDER_SCULPT_SMOOTH_DIAG=1. Ordinary Mesh, symmetry off, Preserve Shape off."
