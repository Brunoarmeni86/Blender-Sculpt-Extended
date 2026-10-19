Extract to C:\BlenderDev\Bake-Cycles-Reconstruction.
No compilation or C++ changes. Uses the already installed Normal HQ and UV fixes.

Developer PowerShell:
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& 'C:\BlenderDev\Bake-Cycles-Reconstruction\TEST-BAKE-CYCLES-RECONSTRUCTION.ps1'

Runs CPU Cycles in fresh background processes (256x256, 16 samples).
Results: cycles-test-DATE-TIME folder, previous/corrected subfolders.
Send cycles-reconstruction-comparison.json plus both render-report.json files.
PNG previews and reconstruction.blend are included for inspection.

This new native test has not run here. Report the first Python error if it stops.
Scalar is exported and checked for normalized seams, not physically reconstructed:
its native normalization scale is not exposed. No guessed scalar strength.
