# CCG-authoritative Sculpt Layer stroke Undo candidate

Parent: 4c7020056c9114fbdb7ff22dd7cf5378594c26df.
User still saw corruption after the synchronization candidate, although behavior
improved. PERSISTENT_V3=0 had previously avoided the reported topology-corruption
artifact. This candidate promotes coordinate-only stroke restore to the default
and retains recorded composition metadata rather than disabling all frame logic.

## Behavior

- BLENDER_SCULPT_LAYER_UNDO_CCG defaults on. Set it to 0 to recover the parent's
  persistent routing; PERSISTENT_V3 controls that legacy route independently.
- No persistent layer payload is captured/exchanged for a CCG-policy stroke.
- Layer frame metadata is captured even with PERSISTENT_V3=0: modifier, layer
  names/order, visibility, strength, active layer and recorded/current level.
- Same-frame/same-level restore uses native compressed_positions exchange and
  marks restored coordinates pending. The normal Multires flush updates deltas.
- When the composition/level differs, use existing recorded-frame conversion:
  flush old live frame, evaluate the recorded frame, exchange coordinates there,
  commit that restored surface, then display the user's frame again.
- No global Commit on each stroke, no full high-resolution layer snapshots.
- Topology-count/layout, stack, L0/session guards remain. This does NOT implement
  a new atomically captured mesh+layer topology undo state or solve same-count
  connectivity changes across Edit Mode. Those require separate validation.
- All-hidden native-base policy is retained. Conform UI is unchanged locally.

## Verification

Compiled the actual policy helper and capture/restore guards with minimal C++
types. Default CCG mode suppresses both persistent paths; CCG=0 preserves the
legacy route. Exact scalar synchronization simulation passes 16 scenarios and
960 coordinate exchanges, covering three consecutive pending strokes, repeated
Undo/Redo and positive/negative/fractional/zero display strengths. Parent legacy
simulation still passes: 32 scenarios, 320 exchanges.

No native Blender build/runtime validation here. These checks cannot reproduce
PBVH boundary stitching, CCG evaluation/reshape, tangent-space interpolation,
BMesh identity remap, or memfile undo. Do not claim all spikes fixed: spikes
caused by incompatible restored states are plausible; geometry conversion
artifacts could remain.

## Runtime validation / rollback

Test with CCG default and all earlier undo overrides removed. On a small copy:
1. Reproduce Edit -> Sculpt -> Undo without Commit and inspect for spikes.
2. Consecutive strokes -> multiple Undo/Redo -> toggle layer -> save/reload.
3. Strength and visibility changes between Undo and Redo.
4. Lower sculpt level and undo after a level change; record timing.
5. All-hidden native strokes, then show layers and undo through that boundary.

CCG=0 selects the previous candidate without rebuilding. Reverse-apply the
isolated production patch to remove this candidate completely. Main is frozen.
