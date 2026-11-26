# D11.9 Windows / Visual Studio 2022 handoff — 2026-11-26

This document records the exact local checkout, build directories, validated commands and current project status so development can continue outside Codex. It is an experimental handoff, not a stable-release declaration.

## Repository checkpoint

- Repository: `Brunoarmeni86/Blender-Sculpt-Extended`
- Source checkout: `C:\BlenderDev\blender-physics-v16`
- Branch: `experiment/physics-v31d11.9-architecture-review`
- HEAD: `34315fecac54e65b159375cc02c6b6da659e8525`
- Commit subject: `D11.9 separate physical brush fields`
- D11.8 preserved checkpoint: `4f5b6a7eb555d864f291ff21e6e1b351e3100c5b`
- Stable release, tag and `main` were not modified or merged.

The D11.9 change separates native write permission, geometric contact, host falloff and material response while preserving the D11.8 numerical envelope when material response is one. This is an architectural preview. It is not yet a production physical-brush implementation.

## Local directory map

| Purpose | Directory |
|---|---|
| Git source checkout | `C:\BlenderDev\blender-physics-v16` |
| Visual Studio 2022 build tree | `C:\BlenderDev\build-physics-v16` |
| Installed runnable build | `C:\BlenderDev\install-physics-v31d11.8` |
| Runnable executable | `C:\BlenderDev\install-physics-v31d11.8\blender.exe` |
| Build-only MSBuild workaround | `C:\BlenderDev\build-physics-v16\Directory.Build.targets` |

The install directory retains the historical `v31d11.8` name, but the installed executable was built from D11.9 commit `34315fecac54`. Check the executable banner/hash rather than inferring its contents from the directory name.

Do not run the raw executable from `build-physics-v16\bin\Release`: its manifest expects the private `blender.crt` and `blender.shared` assemblies produced by the install target. Use the installed executable above.

## Toolchain and configuration

- Generator: `Visual Studio 17 2022`
- MSBuild observed during final install: `17.14.60`
- Compiler toolset: MSVC v143
- `WITH_CYCLES=ON`
- `WITH_CYCLES_DEVICE_CUDA=ON`
- `WITH_CYCLES_DEVICE_OPTIX=ON`
- `WITH_CYCLES_DEVICE_HIP=ON`
- `WITH_CYCLES_CUDA_BINARIES=OFF`
- `WITH_CYCLES_HIP_BINARIES=OFF`
- `WITH_WINDOWS_BUNDLE_CRT=ON`

CUDA/OptiX/HIP device support is compiled into the build, but CUDA and HIP kernels are not shipped as precompiled binaries in this configuration. Do not describe this checkpoint as a fully validated CUDA/HIP binary distribution until Cycles device tests are executed on the target hardware.

### Visual Studio PDB issue and local workaround

After updating Visual Studio 2022, normal `/Zi` compilation failed with compiler error C1902 because the installed compiler/PDB components did not have exactly matching revisions. A direct `/Z7` probe succeeded. The build tree therefore contains this build-only override:

```xml
<Project>
  <ItemDefinitionGroup>
    <ClCompile>
      <DebugInformationFormat>OldStyle</DebugInformationFormat>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
```

File: `C:\BlenderDev\build-physics-v16\Directory.Build.targets`

This forces embedded object debug information (`/Z7`) and avoids the failing external program database writer. It does not modify repository source. Preserve it for incremental builds until the Visual Studio compiler and PDB components are confirmed to match; then test removal in a disposable build tree before changing the working build.

## Reproducible build procedure

Use the Visual Studio bundled CMake explicitly:

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

& $cmake --build C:\BlenderDev\build-physics-v16 `
  --config Release --target blender -- /m:1

& $cmake --build C:\BlenderDev\build-physics-v16 `
  --config Release --target install -- /m:1
```

`/m:1` was deliberately used for the clean validation build to make compiler failures deterministic and avoid PDB contention. Once the PDB mismatch is resolved, parallel incremental builds may be re-evaluated.

Startup validation:

```powershell
& 'C:\BlenderDev\install-physics-v31d11.8\blender.exe' `
  --background --factory-startup `
  --python-expr "import bpy; print('SCULPT_EXTENDED_STARTUP_OK', bpy.app.version_string)"
```

Observed result:

```text
SCULPT_EXTENDED_STARTUP_OK 5.2.3 LTS
Blender 5.2.3 LTS (hash 34315fecac54 built 2026-11-26 12:46:13)
Blender quit
```

## Evidence and validation status

### Actually executed

- Standalone reference suite: 43/43 tests passed in Release before the final full build.
- Clean Visual Studio 2022 `blender` target: completed successfully.
- Visual Studio 2022 `install` target: completed successfully.
- Installed Blender headless startup with factory settings and Python access: completed successfully.
- Repository was clean before writing this handoff.

Non-blocking compiler warnings remain for unused parameters in Sculpt sources (`C4100`). No compiler or linker error remained in the completed build.

### Not yet validated

- Interactive artist test of D11.9 inside the newly installed build.
- Long continuous strokes, thin/vertical surfaces and opposing sheets.
- High-density performance and the 1024-active / 8192-patch preview budget in real Blender strokes.
- Full Undo/Redo transaction behavior.
- Symmetry, radial/tile symmetry and mirrored persistent state.
- Multires, deform modifiers, shape keys and microdetail preservation; the preview currently rejects several of these host cases.
- CUDA, OptiX and HIP Cycles rendering on actual target GPUs.
- Production-ready persistent plastic state in Blender.

Do not convert automated standalone results into runtime claims. The headless startup proves packaging and initialization, not interactive brush quality.

## Recommended continuation

1. Launch the installed executable and perform the manual D11.8/D11.9 A/B on identical meshes and settings.
2. Record environment flags, brush action, density, radius, active/candidate counts, planner time, writer scale and visible result.
3. Test masks, hidden geometry, ordinary symmetry and Undo before widening host compatibility.
4. Keep D11.8 intact; develop further corrections on the D11.9 branch or a new child branch.
5. Integrate the older working Blender/Warp brush branch selectively, commit by commit. Do not perform a blind branch merge: preserve the D11.9 field separation and keep legacy brushes as the default path.
6. Do not enter D12/D13, merge into `main`, publish a release, or rename this preview as stable without explicit approval and runtime evidence.

## Current project assessment

- **Build/packaging:** validated on this Windows machine with the documented `/Z7` workaround.
- **Standalone physical foundation:** automated reference tests passing.
- **D11.9 architecture:** compiled and startup-validated; interactive behavior remains pending.
- **Legacy Sculpt Extended:** intended to remain the default and preserved; no stable release merge occurred.
- **GPU physical brushes:** still experimental and not established by this checkpoint.
- **Multires physical integration:** not completed.
- **Release readiness:** no; this is a developer preview/checkpoint.

## Continuation checkpoint: artistic Warp Material Brushes

The current continuation branch is `experiment/physics-d11.9-multibrush-integration` at commit `43eaca557656` (`Add per-brush Warp material profiles`). It includes earlier D11.9/Warp integration commits and preserves `main`, stable tags and releases.

### Current local outputs

| Purpose | Path |
|---|---|
| Source | `C:\BlenderDev\blender-physics-v16` |
| Incremental VS2022 build | `C:\BlenderDev\build-physics-v16` |
| Runnable Material Brush build | `C:\BlenderDev\install-physics-material-brush-warp` |
| Executable | `C:\BlenderDev\install-physics-material-brush-warp\blender.exe` |
| Artist brush asset library | `C:\BlenderDev\assets-material-brush-warp-v01` |

The final startup check reported Blender 5.2.3 LTS hash `43eaca557656`; `warp.dll` was present. The asset generator wrote 17 brushes and verified the saved `.blend` library. Thumbnail-cache writes under the user profile emitted non-fatal OpenImageIO warnings, while the asset library itself completed successfully.

### Implemented

- Physical material choice is stored per Brush/RNA asset.
- Profiles: Soft Clay, Firm Clay, Mud, Wax, Flesh, Foam, Stone, Wood and Putty.
- D11 material parameters, Clay Strips response and optional Warp finish read the per-brush profile.
- New artist library covers Build, Push, Smear, Gather, Compress, Press, Compact and surface Melt combinations.
- Chisel, Wax Knife and Wood Chisel were removed from the generated library because the current algorithm cannot provide reliable continuous cutting.
- Native brushes remain available and unchanged by default.

### Executed validation

- VS2022/MSVC v143 Blender compilation: passed.
- Clean install into the dedicated Material Brush folder: passed.
- Headless factory startup and RNA enum inspection: passed.
- Warp DLL presence check: passed.
- `sculpt_material_v25_tests`: passed.
- `sculpt_hybrid_foundation_v31d11_tests`: passed.
- `sculpt_brush_solver_backend_tests`: passed.

The complete 44-test suite was not rerun. No interactive sculpt stroke was performed by Codex, so artistic quality, runtime performance, mask/Undo behavior and density consistency remain manual validation, not confirmed fixes.

### How to continue in a normal chat

Give the next assistant this repository instruction:

> Work in `C:\BlenderDev\blender-physics-v16`, repository `Brunoarmeni86/Blender-Sculpt-Extended`, branch `experiment/physics-d11.9-multibrush-integration`. Read `AGENTS.md`, `experimental/sculpt_physics_cpu/SCULPT_ENGINE_MEMO.md` and `doc/sculpt_extended/D11_9_WINDOWS_VS2022_HANDOFF_20261126.md` completely before editing. Verify HEAD is at least `43eaca557656` and check for concurrent or uncommitted changes. Use only Visual Studio 2022/MSVC v143 and preserve the incremental build tree `C:\BlenderDev\build-physics-v16`. The runnable checkpoint is `C:\BlenderDev\install-physics-material-brush-warp\blender.exe`; the asset library is `C:\BlenderDev\assets-material-brush-warp-v01`. Continue the per-brush Warp Material Brush work, keeping native brushes as default and leaving Chisel/Knife disabled. First use my manual A/B observations to tune material differentiation, continuous strokes, narrow features and high-density performance. Preserve D11.9 field separation, Sculpt Layers, Multires data, masks, Face Sets, Undo and PBVH optimizations. Do not modify or merge `main`, publish releases, or claim runtime validation without actual Blender tests. Distinguish code changes, automated tests and my manual artist validation.

Recommended manual matrix: identical 300k and 1M ordinary meshes; Soft/Firm Clay, Mud, Wax, Flesh, Foam and Putty; short dab and long continuous stroke; broad and narrow features; mask boundary; Undo/Redo. Record first-dab delay, steady stroke response, visible material distinction, artifacts and any console `[D11]`/Warp diagnostics. Only then change profile constants or solver stages.

