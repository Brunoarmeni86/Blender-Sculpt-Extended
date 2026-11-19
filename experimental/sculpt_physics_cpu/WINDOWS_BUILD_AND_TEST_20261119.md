# Windows 11 — Build and test Sculpt Physics CPU reference

**19 November 2026.** This builds a **standalone C++17 XPBD library, test executables and optional benchmarks**. It does **not** add a physical Grab brush to Blender or write a `.blend` file. PR #21 is experimental and not merged into the stable release.

## Requirements

- Git for Windows: https://git-scm.com/downloads/win
- CMake: https://cmake.org/download/
- Visual Studio 2022 Community (current supported update), workload **Desktop development with C++**, MSVC compiler and Windows SDK: https://visualstudio.microsoft.com/vs/community/
- PowerShell or **x64 Native Tools Command Prompt for VS 2022**. Close/reopen the terminal after installation so Git and CMake are on `PATH`.

Run `git --version` and `cmake --version` to confirm prerequisites.

## Recommended: isolated experimental checkout

Use a new folder so an existing custom Blender installation or source build is not modified:

```powershell
New-Item -ItemType Directory -Force C:\Dev
Set-Location C:\Dev
git clone --branch experiment/native-physics-assisted-brushes-20261118 https://github.com/Brunoarmeni86/Blender-Sculpt-Extended.git
Set-Location .\Blender-Sculpt-Extended
git status --short
git log -1 --oneline
```

If this repository has **already been cloned here**, skip the clone. To update an isolated checkout with no local edits:

```powershell
git fetch origin
git switch experiment/native-physics-assisted-brushes-20261118
git pull --ff-only
```

Do not use `git reset --hard` to resolve local edits; it can destroy work.

## Standalone CPU build

Run these commands **from the repository root**:

```powershell
cmake -S experimental/sculpt_physics_cpu -B build-sculpt-physics -G "Visual Studio 17 2022" -A x64 -DSCULPT_PHYSICS_BUILD_BENCHMARKS=ON
cmake --build build-sculpt-physics --config Release --parallel 4
ctest --test-dir build-sculpt-physics -C Release --output-on-failure
```

The branch's CI uses equivalent CMake and CTest steps and currently tests **13 standalone suites**, including V11 scale and PBVH-candidate parity tests. If you update to a later revision, the count may change.

To run the optional selection benchmark:

```powershell
.\build-sculpt-physics\Release\bench_candidate_query.exe
```

To run other included CPU benchmarks:

```powershell
.\build-sculpt-physics\Release\bench_stroke_undo.exe
.\build-sculpt-physics\Release\bench_v9_stiffness_grid.exe
```

**Timing scope:** `bench_candidate_query` compares an O(N) full-scan Grab with an indexed Grab whose candidate IDs are already supplied. It does **not** measure the cost of searching PBVH, deforming Blender's evaluated mesh, Undo, viewport refresh, GPU or Multires grids.

## Troubleshooting

- **`cmake` or `git` is not recognized:** install the tool and restart the terminal; inspect `PATH`.
- **Visual Studio generator not found / no C++ compiler:** open Visual Studio Installer → Modify → **Desktop development with C++**, including Windows SDK and MSVC; retry from the VS 2022 x64 developer prompt.
- **CMake generator mismatch from a different build:** choose a *new, empty* build directory such as `build-sculpt-physics-v11`. Never remove a working Blender build directory to fix the standalone prototype.
- **One test fails:** copy the failing suite's output from `ctest --output-on-failure`, plus `git log -1 --oneline`, `cmake --version` and the Visual Studio compiler version. Do not merge the PR based only on some tests passing.
- **Want Debug instead of Release?** Use `--config Debug` with both `cmake --build` and `ctest -C Debug`, and run executables under `build-sculpt-physics\Debug\`.

## Separate task: compiling the full Blender fork

Only do this after the CPU reference succeeds, because it downloads Blender's prebuilt dependencies and performs a much larger build. This fork contains the Blender `make.bat` build entrypoint.

Follow https://developer.blender.org/docs/handbook/building_blender/windows/ and use **an isolated source checkout**, not a distributed release folder.

From that fork's repository root, in the supported Visual Studio developer environment:

```powershell
.\make.bat update
.\make.bat
```

Blender's scripts choose and populate a build directory outside the source tree; the final executable path is printed by the build. Some forks/versions need specific dependencies or compiler versions; rely on the generated errors and official Windows build instructions rather than forcing arbitrary libraries.

**Important:** even after successfully compiling the *full fork* from PR #21, the CPU reference remains isolated under `experimental/sculpt_physics_cpu`. You will see existing Sculpt Extended features but **not a new native XPBD Grab button** until the Blender editor adapter is implemented.

## Recommended first hardware feedback

Run `ctest`, then `bench_candidate_query` and `bench_stroke_undo`. Share the console output and current CPU model/Visual Studio version. Compare p50/p95 and full brush startup once the Blender adapter exists; do not equate this microbenchmark with actual sculpt response.
