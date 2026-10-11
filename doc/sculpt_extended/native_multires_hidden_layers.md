# Native Multires sculpt with all Sculpt Layers hidden

Candidate based on 9ec2f5c1a3b167eb3070b902ca129e92f2ab5697.
Separate from the Conform UI and bake branches; main is untouched.

Admission uses the existing base_has_visible_layers predicate, which checks
visibility flags, not strength. A visible zero-strength layer does not qualify.
When all layers are hidden, pending CCG edits use the same native reshape path
as an empty layer stack. No layer grid or L0 delta is overwritten. Cached layer
evaluation is invalidated after native base reshape.

Layer visibility setters already commit OLD visibility before showing a layer;
that flush now saves the pending base edit before composing visible layers.
Level switches and mode exit likewise retain the existing flush machinery.
Commit reports the native base target when every layer is hidden.

Native-base strokes skip persistent layer snapshots. They retain a small frame
of layer metadata even when PERSISTENT_V3=0, allowing the existing composition
frame path to isolate the hidden base if visibility changes before undo. At an
unchanged frame and level, undo uses native compressed CCG position exchange;
no full-mesh layer snapshot or extra rebuild is introduced. The existing frame
path handles changed composition/level using flush and refresh/rebuild. Its
existing L0-frame safeguards and modifier/topology checks remain in place.

Validation: standalone C++ routing test compiles actual admission, flush and
snapshot guards plus the existing visibility predicate. Five cases cover empty,
one/two hidden layers, visible zero strength, and mixed visibility; mock reshape
failure is checked and stored layer payloads remain unchanged. This checks
routing, NOT real reshape geometry, Blender ABI, native Undo or runtime timing.
Blender compilation and runtime validation are pending.

Windows validation on a copy:
1. Create a layer, sculpt distinct detail with REC, Commit, and hide all layers.
2. Turn Sculpt Base Mesh off, keep Sculpt Level above zero, and sculpt the native
   Multires. Test Undo/Redo while all layers remain hidden.
3. Commit; show the layer and check both native edits and original layer detail.
4. Hide again: native edits must remain, original layer detail must disappear.
5. Repeat below max level; change levels, exit/reenter Sculpt, save/reload.
6. Check undo around visibility changes separately and record any failure.
7. One visible zero-strength layer must still require normal REC/valid weight.

Use PERSISTENT_V3=0 for the initial test, matching the user's current diagnostic
configuration. General Edit-to-Sculpt persistent Undo corruption remains open.
