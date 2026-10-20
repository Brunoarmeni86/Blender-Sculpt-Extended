# Sculpt Extended: native Windows shareable candidate, 20 October 2026

The native candidate starts directly from `blender.exe`, with the previously tested feature profile initialized before Blender startup and before feature caches or worker threads. The Face Sets / Polypaint panels are distributed through `bl_ui`; no external UI script or PowerShell launcher is needed to use the candidate.

## Scope

The native integration changes only `source/creator/creator.cc`, a local profile header, `bl_ui/__init__.py`, and the new Sculpt Extended UI module. It retains the current geometric kernels, spike handling, Sculpt Layers, bake, transport, Face Sets and Polypaint data formats. The 27 defaults match the tested launcher. Explicit environment overrides remain effective; `BLENDER_SCULPT_EXTENDED_NATIVE=0` skips installing defaults. Diagnostic and superseded experimental paths remain off.

Existing Face Sets eligibility restrictions (single Multires, supported resolution and topology) are retained. The integration does not add support beyond the checked implementation. L0 color is a filtered preview of the retained HR master; the dense detail is not deleted or written back by the renderer.

## Build and package

Run `python -u tools/sculpt_extended/native_release/RELEASE.py` on the Windows machine with the existing configured source/build directories. Defaults are `C:\BlenderDev\blender` and `C:\BlenderDev\build_windows_x64_shareable`. The script validates all patch contexts and baseline hashes, backs up changed sources and the current executable, builds Release with the existing GPU/toolchain configuration, and runs `cmake --install` into a new staging directory. It does not reconfigure GPU support or replace third-party dependencies.

A focused test starts the staged executable without activation flags or external UI, verifies registered panels/properties and native defaults, then checks actual L0 viewport color, read-only invariants, Paint, fine residual, native Undo/Redo and save/reopen. A ZIP of the entire installed runtime is created only after PASS. The runtime has its own `portable` directory. Report and ZIP/executable SHA256 values are written next to the package.

Restore the integration with `python -u RELEASE.py --restore PATH_TO_BACKUP` while Blender/build tools are closed. That restores the changed source files and previous executable; the next build recompiles restored sources. Generated runtime dependencies are supplied by the existing installation and are not replaced by this source patch.

## Validation status

The preceding Windows snapshot is verified in [VERIFIED_20261019.md](VERIFIED_20261019.md). Local checks of this new integration pass: exact patch replay/idempotence, Python parsing, UI import/property lifecycle with API doubles, and isolated compilation of the exact profile initializer under ASan/UBSan (27 defaults, explicit overrides and opt-out).

**The native integration has not yet been compiled or viewport-tested on Windows.** The build/test tool records this separately and will stop before packaging if the native test fails. Local tests are not a substitute for the native build. Large-mesh performance and GPU runtime behavior are not certified by the focused L0 test.
