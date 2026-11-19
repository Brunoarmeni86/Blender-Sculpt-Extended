# Native Cycles diagnostic: mixed results, not reconstruction validation

User supplied three comparison files. The unnumbered and (1) copies are byte
identical (SHA256 83f57ecc5866d4a029a24aa023ccd91b4a14e3c73c37f110134853d6197a8b6d).
The (2) copy is a second distinct result (SHA256
6e2fb9a482ffcbc35e79bf78ad7d1892fc89a689e442251f54e9ac05c2d3f5ab).
Both unique reports are retained verbatim with repository-safe names.
No raw float renders, separate render reports or saved scenes were supplied
in this upload; numbers below are report analysis, not independent pixel recomputation.

The native renders and comparator ran. Both reports say needs_scene_mismatch_review:
exact saved reference mesh/camera fingerprints differ. Image reference drift is
very small: RMS angle 0.0000735 / 0.0001734 degrees, alpha difference zero,
maximum RGBA difference 0.0000159 / 0.0000236. This is consistent with small
numerical variation, but hashes alone do not localize it or prove its cause.

Both reports agree closely on results. Run 1:

| Metric | Previous HQ | Corrected HQ |
| --- | ---: | ---: |
| Normal seam angle RMS (90 samples), degrees | 0.935602 | 0.499450 |
| Normal seam maximum, degrees | 2.870929 | 1.553153 |
| Normal-only render vs high RMS, degrees | 11.380154 | 11.700175 |
| Normal + Vector Object vs high RMS, degrees | 21.083143 | 21.276385 |
| Normal + Vector Tangent vs high RMS, degrees | 38.471098 | 38.561403 |

Normal seam diagnostic improves by 46.6%. Vector Object and normalized scalar
seam jumps remain essentially unchanged, consistent with Normal-only production
scope. Cross-control reference comparisons give the same broad render outcome;
reference pixel drift is far smaller than the candidate differences. It must not
be used to dismiss or reverse the observed render trend.

The rendered comparison does NOT improve: all three screen-space errors rise
slightly. Large absolute errors already exist in the prior route, especially
Vector Tangent. The test includes changed geometry/view correspondence and Simple
subdivision tangent frames, so it does not isolate the Normal conversion. These
reports neither certify complete reconstruction nor establish that reverting
the Normal fix would solve the dominant error. Prior 216-frame isolated tests,
120 native center-reference tests and improved seam probes retain their stated
limited validity.

Next useful diagnostics: measure numerical mesh/camera differences rather than
exact hashes; isolate normal shader conversion on identical geometry and surface
coordinates; then audit geometry reconstruction independently, including Vector
Tangent's actual Cycles basis, padding and corner-normal conventions. Do not
increase global sample count to treat this as sampling noise. Main stays frozen;
PR remains draft; no production changes in this evidence commit.
