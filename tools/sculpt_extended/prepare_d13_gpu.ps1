# Export and activate the existing artistic CUDA kernels. Run in the shell
# that will launch Blender. Does not rebuild or overwrite Blender executable.
[CmdletBinding()]
param(
 [string]$Source = "C:\BlenderDev\blender-physics-v16",
 [string]$InstallDirectory = "C:\BlenderDev\install-physics-d13-recovery",
 [string]$WarpRoot = "C:\BlenderDev\third_party\warp-1.19.0",
 [string]$Python = "C:\BlenderDev\blender\lib\windows_x64\python\313\bin\python.exe"
)
$ErrorActionPreference = "Stop"
foreach($p in @($Python,(Join-Path $InstallDirectory "blender.exe"),(Join-Path $WarpRoot "warp\bin\warp.dll"))){
 if(!(Test-Path -LiteralPath $p)){throw "Missing: $p"}
}
$env:PYTHONPATH=$WarpRoot
$staging=Join-Path ([IO.Path]::GetTempPath()) ("bse-d13-"+[Guid]::NewGuid().ToString("N"))
try {
 & $Python (Join-Path $Source "experimental\sculpt_physics_gpu\export_artistic_native_d13.py") --output $staging
 if($LASTEXITCODE -ne 0){throw "Artistic CUDA fixture/export failed"}
 foreach($stem in @("artistic_pressure","artistic_flux")){
  $ptx=Join-Path $staging "$stem.ptx"
  $manifest=Join-Path $staging "$stem.symbols"
  $symbol=(Get-Content -LiteralPath $manifest -Raw).Trim()
  if($symbol -notmatch "^artistic_\w+_cuda_kernel_forward$" -or
      (Get-Content -LiteralPath $ptx -Raw) -notmatch ("\.entry\s+"+[regex]::Escape($symbol)+"\s*\(")){
   throw "Invalid PTX entry $stem"
  }
 }
 foreach($name in @("artistic_pressure.ptx","artistic_pressure.symbols","artistic_flux.ptx","artistic_flux.symbols")){
  Copy-Item -LiteralPath (Join-Path $staging $name) -Destination (Join-Path $InstallDirectory $name) -Force
 }
} finally {
 if(Test-Path -LiteralPath $staging){Remove-Item -LiteralPath $staging -Recurse -Force}
}
$env:BLENDER_SCULPT_D11_WARP_PTX=Join-Path $InstallDirectory "artistic_pressure.ptx"
$env:BLENDER_SCULPT_D11_WARP_KERNEL=(Get-Content -LiteralPath (Join-Path $InstallDirectory "artistic_pressure.symbols") -Raw).Trim()
$env:BLENDER_SCULPT_D11_FLUX_PTX=Join-Path $InstallDirectory "artistic_flux.ptx"
$env:BLENDER_SCULPT_D11_FLUX_KERNEL=(Get-Content -LiteralPath (Join-Path $InstallDirectory "artistic_flux.symbols") -Raw).Trim()
$env:BLENDER_SCULPT_D11_WARP="1"
Write-Host "[D13] Artistic pressure and flux modules staged and shell configured. Restart Blender before using."
