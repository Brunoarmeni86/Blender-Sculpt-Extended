param(
  [Parameter(Mandatory = $true)] [string] $BuildDirectory,
  [Parameter(Mandatory = $true)] [string] $InstallDirectory,
  [string] $LibraryDirectory = "C:\BlenderDev\blender\lib\windows_x64",
  [string] $HipDirectory = "C:\Program Files\AMD\ROCm\7.2",
  [string] $OptixDirectory = "C:\ProgramData\NVIDIA Corporation\OptiX SDK 8.0.0"
)

$ErrorActionPreference = "Stop"
$sourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

foreach ($required in @($cmake, $msbuild, $LibraryDirectory, $HipDirectory, $OptixDirectory)) {
  if (-not (Test-Path -LiteralPath $required)) {
    throw "Required build dependency is missing: $required"
  }
}

$lfsRoots = @((Join-Path $sourceDirectory "release"), (Join-Path $sourceDirectory "scripts"))
$ripgrep = Get-Command rg -ErrorAction SilentlyContinue
if ($ripgrep) {
  $lfsPointers = & $ripgrep.Source -l --fixed-strings "version https://git-lfs.github.com/spec/v1" $lfsRoots
}
else {
  $lfsPointers = Get-ChildItem -LiteralPath $lfsRoots -File -Recurse |
    Select-String -SimpleMatch "version https://git-lfs.github.com/spec/v1" -List |
    Select-Object -ExpandProperty Path
}
if ($lfsPointers) {
  throw "Git-LFS assets are not materialized. Run: git lfs pull"
}

if ((Test-Path -LiteralPath $BuildDirectory) -or (Test-Path -LiteralPath $InstallDirectory)) {
  throw "For a clean build, BuildDirectory and InstallDirectory must not already exist."
}

& $cmake -S $sourceDirectory -B $BuildDirectory -G "Visual Studio 17 2022" -A x64 `
  "-DLIBDIR=$LibraryDirectory" `
  "-DCMAKE_INSTALL_PREFIX=$InstallDirectory" `
  -DWITH_CYCLES=ON `
  -DWITH_CYCLES_DEVICE_CUDA=ON `
  -DWITH_CYCLES_DEVICE_OPTIX=ON `
  -DWITH_CYCLES_DEVICE_HIP=ON `
  -DWITH_CYCLES_DEVICE_HIPRT=ON `
  -DWITH_CYCLES_CUDA_BINARIES=ON `
  -DWITH_CYCLES_HIP_BINARIES=ON `
  "-DHIP_ROOT_DIR=$HipDirectory" `
  "-DHIPRT_ROOT_DIR=$LibraryDirectory\hiprt" `
  "-DOPTIX_ROOT_DIR=$OptixDirectory"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed: $LASTEXITCODE" }

$env:CL_MPCount = "2"
& $msbuild (Join-Path $BuildDirectory "INSTALL.vcxproj") /m:2 /p:Configuration=Release /p:Platform=x64 /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Visual Studio 2022 build failed: $LASTEXITCODE" }

& (Join-Path $InstallDirectory "blender.exe") --background --factory-startup --python-expr `
  "import bpy; assert bpy.app.build_options.cycles; assert bpy.app.build_options.openvdb; print(bpy.app.version_string)"
if ($LASTEXITCODE -ne 0) { throw "Installed Blender smoke test failed: $LASTEXITCODE" }

Write-Host "Clean VS2022 GPU build completed: $InstallDirectory"
