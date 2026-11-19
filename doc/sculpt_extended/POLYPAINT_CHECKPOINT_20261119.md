# Polypaint performance checkpoint — 19 November 2026

Branch: `fix/preserve-other-layers-smooth-contract-v01`

## Included changes

- The Paint path reads exact Multires master samples directly when possible.
- Paint avoids the generic Blur/Smear neighbor graph and the global active-index sort.
- Interior CCG samples bypass seam grouping and its visited hash table.
- Paint retains before/after colors only for samples that actually change.
- The fixed active-sample rejection was removed for Paint. Blur and Smear keep their bounded graph path.
- Polypaint Fill has a registered panel section, a toolbar bucket tool, its own color selector and Undo/Redo support.
- The native regression harness accepts `SCULPT_TEST_PAINT_SIZE` for broad-stroke checks.

## Evidence

- `paint_levels`: Blender runtime validated at L0 through L4 before the final removal of the Paint sample cap. Exact master presence, base color, Undo and Redo passed.
- Dense fixture above one million master samples: Blender runtime validated before the final removal of the Paint sample cap.
- On the same dense fixture, the Paint `calculate` phase decreased from approximately 0.17–0.19 ms to approximately 0.12 ms. The first dab decreased from 5.74 ms to 4.38 ms.
- Broad Paint with about 148,000 changed samples completed before the final removal of the cap.
- The final no-cap Paint change is MSVC 2022 compile validated. Runtime validation is intentionally left to the user.

## GPU build configuration

The Windows build was configured with:

- `WITH_CYCLES_DEVICE_CUDA=ON`
- `WITH_CYCLES_CUDA_BINARIES=ON`
- `CYCLES_CUDA_BINARIES_ARCH=sm_86` for the installed RTX 3080
- `WITH_CYCLES_DEVICE_OPTIX=ON`
- OptiX SDK 8.0.0

CUDA `kernel_sm_86.cubin.zst` and the OptiX PTX modules compiled and were installed. Cycles CUDA/OptiX runtime rendering was not tested in this checkpoint at the user's request.

## Stop decision

The remaining dominant work is factor collection. A deeper optimization would require a streamed per-grid Paint pipeline and new atomic publication/rollback handling across CCG seams. That rewrite is deferred because its correctness risk to masks, seams and Undo is larger than the currently measured gain.
