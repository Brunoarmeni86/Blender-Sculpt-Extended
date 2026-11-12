# Windows clean build contract

Sculpt Extended Windows builds must use **Visual Studio 2022 / MSVC v143**. Visual Studio
2026 is not supported by CUDA 12.8 and must not be used for this project.

Before configuring a build:

1. Run `git lfs pull` and verify that no Git-LFS pointer text remains under `release/` or
   `scripts/`.
2. Use new, empty build and install directories for a release candidate.
   Keep both paths short (the validated root is `C:\BlenderDev`) because generated shader
   filenames can otherwise exceed MSVC/Windows path limits.
3. Keep CUDA, OptiX, HIP and HIPRT enabled only when their SDK checks pass.
4. Build the CMake `INSTALL` target, not only `blender.exe`, so runtime libraries and data
   files are packaged together.
5. Test both headless and graphical startup from the installed directory.
6. Scan the installed tree and final ZIP files for unresolved Git-LFS pointers and exclude
   development symbols and link libraries.

The reproducible entry point is:

```powershell
tools\sculpt_extended\build_windows_vs2022_gpu.ps1 `
  -BuildDirectory C:\path\to\new-build `
  -InstallDirectory C:\path\to\new-install
```

The script refuses existing output directories, missing SDKs and unresolved Git-LFS assets.
It limits MSBuild and compiler parallelism to two workers; higher automatic parallelism has
produced intermittent MSVC `D8040`/`C1083` resource failures on this workstation.
Subsequent development builds may be incremental inside the validated VS2022 build directory;
distribution checkpoints must start from new directories.
