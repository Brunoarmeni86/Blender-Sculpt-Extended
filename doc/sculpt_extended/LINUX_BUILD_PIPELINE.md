# Linux build cache and recoverable verification

The `Linux Rocky 8 cached build and verification` workflow is started manually.
It keeps the approved Rocky Linux 8/glibc 2.28 baseline, GCC 14, three compiler
processes, Release configuration and the CUDA/OptiX, HIP and oneAPI backends.
It does not install Linux tooling on the user's Windows PC.

## Build a new source revision

Open Actions, select **Linux Rocky 8 cached build and verification**, then
**Run workflow**. Supply the exact approved 40-character source commit in
`source_sha`. Leave `snapshot_run` empty.

The build job restores a native C/C++ `ccache` selected by compiler content,
library revision and Release preset. The cache is limited to 4 GB. A different
source commit can reuse compatible entries through the same fingerprint prefix;
ccache still checks compiler options, source and headers for each compilation.
Cache keys are scoped to the same workflow branch or the repository default
branch under GitHub's normal rules. No cache quota or paid runner is enabled.

Cache saving happens after successful compilation and before package checks.
The completed portable package, checksum, provenance and build configuration are
uploaded as `Linux-UNVERIFIED-<run ID>`. These artifacts are **not release assets**.
The verification job independently downloads the snapshot, checks the source and
checksum, runs startup/brush/mask regressions and inspects GPU kernels and glibc
requirements. Only then does it upload the normal verified artifact.

## Retry checks without rebuilding Blender

- For a transient failure, use **Re-run failed jobs** on the original run. The
  verification job downloads the original snapshot, including on later attempts.
- If a verification script itself must be corrected, update the workflow, start
  a new run with the same `source_sha` and supply the original build run ID in
  `snapshot_run`. The build job is skipped; the updated verification job checks
  the existing snapshot. Its source commit and checksum must still match.

Unverified snapshots are retained for 7 days. Once expired, they cannot be used
for recovery. Retry of an entire run can replace its own intermediate CI
artifacts; published releases and release tags are never modified by this
workflow. Ubuntu 24.04 startup is still checked by the publication workflow.
Physical GPU hardware is not tested by these runners.

## Validation and limits

`Validate Linux cache and recovery pipeline` lints the workflows and performs a
small C++ compilation in Rocky Linux 8. It saves the compiler cache and an
unverified artifact, restores both on a separate runner, requires a real compiler
cache hit and verifies the object checksum. It does not rebuild Blender.

The first full run of the new workflow starts with a cold cache. The existing
Linux run was not interrupted and cannot retroactively populate this cache.
Real Blender rebuild speedups are measured with `ccache --show-stats`; native
cache tests do not establish a Cycles kernel cache speedup. Kernel-specific reuse
and larger paid runners are outside this change.

The PC must only enter standby on a new explicit user request. Build completion
must not trigger standby automatically.

The lightweight workflow completed successfully in [run 40130187355](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/actions/runs/40130187355): workflow lint, native cache hit, cache persistence across separate runners and recovery of the compiled artifact with matching checksum all passed. A full Blender build with this new workflow has not yet been benchmarked.
