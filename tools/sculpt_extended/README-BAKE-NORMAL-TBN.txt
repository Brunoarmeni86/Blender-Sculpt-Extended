Normal HQ TBN candidate, on top of the tested CPU UV coverage patch.
Do not remove the earlier coverage patch. No GPU or Vector changes.

Extract this folder to C:\BlenderDev\Bake-Normal-TBN.
In your existing Visual Studio Developer PowerShell:
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& 'C:\BlenderDev\Bake-Normal-TBN\APPLY-BAKE-NORMAL-TBN.ps1'

After build/install, close the opened Blender and run:
& 'C:\BlenderDev\Bake-Normal-TBN\TEST-BAKE-NORMAL-TBN.ps1'

Return native-normal-comparison.json and the three native-normal-*.json reports.
Native probe not run by the author: inspect errors if Blender RNA differs.
The 1x center reference is a diagnostic, not a complete render accuracy oracle.

Undo only this patch and rebuild:
& 'C:\BlenderDev\Bake-Normal-TBN\APPLY-BAKE-NORMAL-TBN.ps1' -Revert
