# Windows native Normal HQ TBN probe: 2026-10-12

User-supplied reports from Blender 5.2.1 LTS after correcting the probe UV map.
Production candidate: 9842a175; probe correction: 8f4eb85. Binary SHA256:
D5203689BC189F5A2AD66B746D53A8B721687E0CB0CBC3D14265621F9CB105C8.
The hash identifies the tested executable; reports do not embed its source commit.

All three reports contain 120 matching face/pixel probes, identical frame hash,
active UV TBNProbe, finite RGB/object normals, and alpha 1. Mode metadata is
reference SS=1/TBN=1, previous SS=4/TBN=0, corrected SS=4/TBN=1.
Independent recalculation from uploaded normals agrees with the comparison.

| Relative to 1x center reference | Previous HQ | Corrected HQ |
| --- | ---: | ---: |
| RMS angle, degrees | 1.083002470 | 0.285127835 |
| Maximum angle, degrees | 2.666404041 | 0.872638015 |
| Mean angle, degrees | 0.962659575 | 0.233733856 |

RMS decreases by about 73.7%; 114/120 samples are closer to the center reference.
Regular UV islands: RMS 1.187985161 -> 0.262990896, 57/60 improve.
Mirrored UV islands: RMS 0.966685034 -> 0.305665754, 57/60 improve.
Six samples are not closer; 4x4 averages a footprint while 1x samples its center,
so monotonic per-pixel convergence is not a required filter property.

This confirms the native bake operator and TBN switch execute successfully and
improve this diagnostic. It is not a full Cycles render reconstruction validation,
sculpt-detail oracle, custom/split-normal validation, transformed-object test,
Vector/scalar displacement validation, seam/padding test or performance benchmark.
Keep PR draft pending broader validation. No production code changed here.

Original JSON reports are retained verbatim. The binary hash text was transcoded
from Windows UTF-16 to UTF-8 for repository readability.
