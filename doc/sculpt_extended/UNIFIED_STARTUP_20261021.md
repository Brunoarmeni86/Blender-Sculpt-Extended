# Unified Sculpt Extended startup — 2026-10-21

> Superseded by `APPROVED_V05_COMPLETE_20261021.md`. This historical launcher-parity profile omitted required V05 optimization defaults and reintroduced the legacy REC UI. Its original 74-default validation is not the acceptance test for the corrected build; use `tools/sculpt_extended/approved_v05_20261021/validate.py`.

Opening `blender.exe` now enables the same functional profile and Multires sidebar that previously required `AVVIA-SCULPT-EXTENDED.cmd`. The native profile contained only 29 defaults; three nested PowerShell launchers supplied 52 additional or overlapping settings, and a Python session script registered four extra UI classes and a diagnostics operator.

The creator profile now supplies 74 defaults before Python and feature caches initialize. Explicit environment overrides and `BLENDER_SCULPT_EXTENDED_NATIVE=0` remain supported. The four existing sidebar classes are registered through `bl_ui`; the diagnostic operator is registered normally and writes its reports to the temporary directory instead of the installation directory. Geometric kernels and Face Sets algorithms are unchanged.

Three intentional differences from the old launcher are recorded in the validation kit: `BLENDER_SCULPT_COMMIT_KEEP_CCG=0` preserves the established geometry safety constraint; layer response tracing and subdivision profiling default to zero. No unverified V05 optimization profile has been substituted for the user's working launcher profile.

The compatibility CMD invokes `blender.exe` directly. The installed application and `.blend` file opening therefore use the native profile without a session-only Python loader. The legacy launcher hierarchy is excluded from the runtime-only distribution, along with development symbols, linker files, build utilities, Python bytecode caches and startup backups. Standard DLLs, Python, assets, licenses and Blender resources are retained.

## Validation

The Windows Release executable was rebuilt incrementally; this is not a claim of a full clean rebuild of all dependencies. The validation runner clears inherited feature flags and checks normal native startup, explicit overrides and opt-out. Normal startup also exercises two subdivisions, layer creation/duplication, hidden-layer state and save/reopen. All three modes passed. No new interactive viewport/GPU test or full stroke/Bake Undo/Redo regression was performed.

Run against an installed build or extracted clean portable:

```powershell
python tools/sculpt_extended/unified_startup_20261021/run_tests.py --blender C:/path/to/blender.exe --output C:/path/to/test-results
```

`package_portable.py` takes `--installed`, a new `--output` directory, `--archive` and `--source-commit`. It verifies the ZIP against every packaged file. `BUILD-MANIFEST.json` in the distribution records source hashes and executable hash; the embedded Blender Git hash remains `unknown` because the existing build tree compiles a source copy without Git metadata. A new build configured from a Git checkout is needed to restore embedded Git provenance.

The previous portable ZIP is retained locally for rollback; the frozen V05 checkpoint and its independent results remain separate.
