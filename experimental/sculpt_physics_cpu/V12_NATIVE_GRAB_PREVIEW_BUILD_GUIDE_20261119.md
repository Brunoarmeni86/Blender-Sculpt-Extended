# V12 first native Sculpt Grab integration — Windows build and safety gate

**19 November 2026. Development-only, UNVERIFIED full Blender editor build.** This is a separate experimental branch based on the 14-suite V11 CPU reference, not a released feature. Build with care; back up your files. **The native Blender code has not yet passed a complete Blender/Windows compile.** The 15 standalone CPU CTest suites do not compile `grab.cc`.

## V12 implementation scope

- The original Blender Grab code remains the default. CMake option `WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL` is **OFF** by default. Even when compiled ON, the experimental branch is runtime OFF unless environment variable `BLENDER_SCULPT_XPBD_GRAB_V12=1`.
- When both gates are ON, Grab's existing PBVH Mesh-node selection and `calc_factors_common_from_orig_data_mesh()` supply factors from **Blender's native masks, textures, automask and brush falloff**, rather than a parallel falloff formula. The new `World::set_brush_targets()` receives explicit sorted goal positions, preserving the distinction between editor responsibilities and solver mathematics.
- The solver is a fresh **quasistatic XPBD proxy per dab**, with distance bonds derived from source mesh edges. It uses `bond_hops=0`: the exterior mesh is a **numerical pin**, and only target vertices inside the already-Undo-covered PBVH `node_mask` are allowed to move. This is intentionally conservative; it avoids touching other Blender vertices whose native Undo nodes were not recorded. It is **not** a dynamic whole-surface clay solver.
- Blender `PositionDeformData::deform()` handles writeback to normal mesh Sculpt. All node translations are staged, their finite float range validated, and axis-lock/mirror clipping is applied before writes. PBVH nodes are updated and tagged exactly through the original `node_mask`.
- On an exception **before writeback**, the experimental path disables itself for the rest of that stroke and falls back to original Grab. Existing Sculpt brushes are unchanged when either gate is OFF.

### Explicit unsupported paths (original Blender Grab remains in charge)

- Multires grids or any object with a Multires base modifier / Sculpt Layers.
- Dyntopo/BMesh, shape keys, deforming modifiers, symmetry/radial symmetry, tiled passes.
- Meshes above **20,000 vertices**.
- Physics-only volumetric clay, self-contact, realistic friction, fracture-surface reconstruction, persistent multi-dab momentum. All require further work.
- No user-facing panel/button yet; this gate uses one compile option + one environment variable intentionally.

## First validation: standalone C++ library (fast)

Open PowerShell:

```powershell
git clone --branch experiment/sculpt-physics-native-grab-v12-20261119 https://github.com/Brunoarmeni86/Blender-Sculpt-Extended.git C:\Dev\Sculpt-XPBD-V12
Set-Location C:\Dev\Sculpt-XPBD-V12
cmake -S experimental/sculpt_physics_cpu -B build-physics -G "Visual Studio 17 2022" -A x64
cmake --build build-physics --config Release --parallel 4
ctest --test-dir build-physics -C Release --output-on-failure
```

The 15th standalone suite checks new editor-authored goal targets, fixed/masked particles, sorted targets, duplicate/invalid rollback and refusal to change topology during an active stroke.

## Full Blender fork build (large!)

**Prerequisites:** Visual Studio 2022 with *Desktop development with C++*, Windows SDK, CMake, Git, a suitably recent Windows toolchain and space for Blender source + downloaded dependencies. Review https://developer.blender.org/docs/handbook/building_blender/windows/.

In PowerShell from the V12 repo root:

```powershell
Set-Location C:\Dev\Sculpt-XPBD-V12
.\make.bat update
.\make.bat
```

The first build uses the existing default path, **without** experimental physics. It establishes a baseline and downloads dependencies. Resolve any standard build errors first. Do not reuse the directory of your stable released Blender.

Locate the generated build directory containing `CMakeCache.txt`, typically somewhere under `C:\Dev`:

```powershell
Get-ChildItem C:\Dev -Filter CMakeCache.txt -Recurse -ErrorAction SilentlyContinue |
    Select-Object FullName
```

Choose the `CMakeCache.txt` for **this Blender source checkout** (not `build-physics`). Replace the placeholder below with its **parent directory**:

```powershell
$build = "C:\Dev\YOUR_BLENDER_BUILD_DIRECTORY"
cmake -S C:\Dev\Sculpt-XPBD-V12 -B $build -DWITH_SCULPT_XPBD_GRAB_EXPERIMENTAL=ON
cmake --build $build --config Release --target blender --parallel 4
```

This explicitly compiles the experimental `physics_core.cc` into the editor library and turns on the alternative Grab code path. Without `-DWITH_SCULPT_XPBD_GRAB_EXPERIMENTAL=ON`, there is **no** alternative code in the Blender executable.

Find the newly compiled executable, then start a **separate** process with the runtime gate:

```powershell
Get-ChildItem $build -Filter blender.exe -Recurse -ErrorAction SilentlyContinue |
    Select-Object FullName

$env:BLENDER_SCULPT_XPBD_GRAB_V12 = "1"
& "C:\Dev\YOUR_BLENDER_BUILD_DIRECTORY\bin\Release\blender.exe"
```

The last executable path is **illustrative**; use the path returned by `Get-ChildItem`. For ordinary Blender comparison, clear the environment variable and restart:

```powershell
Remove-Item Env:\BLENDER_SCULPT_XPBD_GRAB_V12 -ErrorAction SilentlyContinue
```

## Safest viewport test

1. Back up/duplicate your work; use a **new test scene** with a basic subdivided sphere or simple organic mesh **under 20k vertices**. No Multires modifier, Sculpt Layers, symmetry, shape keys or deform modifiers. Do not test on production assets.
2. Enter Sculpt Mode and choose Blender's standard **Grab** brush. With experimental build and environment enabled, its regular PBVH Mesh path should invoke the XPBD preview; there is no new button yet.
3. Perform short single dabs, then a slow drag. Watch the deformation and console for `[V12 XPBD Grab]` errors. Use native Ctrl+Z and compare vertex positions with regular Grab in a separate run.
4. Record visible jumps, slow strokes, odd masking behavior, and whether Undo restores *all* geometry after crossing PBVH node boundaries.
5. Test exact bypass: runtime env removed/disabled, or a normal build with the feature compile option OFF, must behave exactly like the original brush.

**Stop and share compile logs if the full Blender build fails.** The optional path has not been compiled against Blender's full editor toolchain yet; the CPU reference test result cannot validate CMake integration, `PositionDeformData` calls, masking, or PBVH writeback.

## Architectural limitations / next native pass

- **Performance:** the World and every edge bond are reconstructed each dab. This is O(N+E) per stroke sample and **not usable as a high-resolution production brush**. Next gate: stroke-owned persistent proxy with topology generation, indexed dirty-state synchronization and precomputed adjacency.
- **Physics:** only vertices directly targeted by Blender factor sampling are dynamic in this first preview (`bond_hops=0`), with boundary nodes pinned; no verified momentum between dabs. Build a target-driven, multi-island proxy with native Undo coverage once the preview passes.
- **Masked vertices:** completely masked/hidden source vertices are pinned; partial masks contribute to native target factors and inverse mass. Physics-induced motion and masks must be validated further, especially Face Sets, automasking and high-frequency details.
- **Safety:** native editor undo/redo, modifier/crazyspace, CCG dirty tiles, layer mutation, mesh save/reload and cross-branch compatibility have not been validated. The runtime flag is an experimental gate, not a safety certification.
- **Precision:** distance bonds alone do not constitute a realistic clay material; volumetric constraints, friction and adaptive interface reaction validation remain independent tasks.
- **Engineering:** before turning this into an artist-facing option, build a native interface adapter for Blender's already-existing XPBD distance constraint implementation and add separate full Blender build CI. Do not fork two permanent physics solvers.

**Do not merge V12 into main or into the stable V11 reference until the full Blender build and viewport tests pass.**
