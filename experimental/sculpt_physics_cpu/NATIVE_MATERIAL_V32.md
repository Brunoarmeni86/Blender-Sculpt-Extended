# V32 — Native Material Response
Base: V31-D3 6b8b6a05cc30 (preserved on its original branch).

## Six implementation stages
1. Native Clay and Clay Strips footprint and plane, including native pressure, masks, falloff, texture, trim, accumulation and native bounding/Undo selection. No V31 footprint replacement in V32.
2. Exact stock reference via BLENDER_SCULPT_V32_MODE=NATIVE for explicit Clay-family assets. Other stock brushes retain their existing route.
3. Capture finished native translations before editor write, using the already established V15 bridge. In MATERIAL, no autonomous solver, no old contact slab, no mandatory XPBD. Normal native clip/lock and PBVH bounds/writeback. Unsupported geometry falls through to stock, rather than forcing the legacy material solver.
4. Eight qualitative family profiles (Clay/Wax/Stone/Wood/Flesh/Mud/Fabric/Foam), bounded gain near stock; explicit material amounts, optional cohesion/viscosity/stiffness/plasticity overrides. Family recognized by V32 asset name or process override. Not a constitutive law; no persistent elastic return or hardening yet. Native specialized tools retained as stock reference, not silently routed to Clay.
5. Optional local redistribution of already nonzero native target vectors over real mesh edges, weighted by positive signed projected dual areas. No expansion into zero-target or detached sheets; cap correction at 30% of native response and reject if real incident-triangle guard would scale it. Conservation is integrated vector target before native clipping, not exact volume or tangential particle transport. Default transport off for stock parity. Native clipping can change the budget.
6. XPBD mode routes native material targets to the existing compact CPU compliant executor, current rest geometry and default .20 assist. Existing symmetry/Multires/modifier and clipping limitations remain; no new GPU executor or volumetric solver claimed. SIMULATION (or unset) retains old V31 autonomous dispatcher; select legacy flags separately. Warp prototypes already in gpu/; keep isolated pending parity and latency gates below.

## Environment controls
- BLENDER_SCULPT_V32_MODE=NATIVE | MATERIAL | XPBD | SIMULATION
- BLENDER_SCULPT_V32_RESPONSE=.65 (0 exact stock in MATERIAL)
- BLENDER_SCULPT_V32_FAMILY=AUTO (asset prefix, otherwise Clay; explicit family override supported)
- BLENDER_SCULPT_V32_TRANSPORT=0 (0..1; scale of profile-specific small correction)
- BLENDER_SCULPT_V32_XPBD_ASSIST=.20 (0..1)
- Optional BLENDER_SCULPT_V32_{PLASTICITY,COHESION,VISCOSITY,STIFFNESS} (0..1).
- BLENDER_SCULPT_PROFILE_V26=1 enables [V32 native] diagnostics.
Do not enable broad old XPBD environment on all stock brushes for native A/B.
The material bridge is Mesh only; native specialized recipes do not use V32 response.
Current implementation uses a serial gather/write, benchmark on dense PBVH and symmetry.
The asset library generator generate_material_brushes_v32.py creates 31 NEW recipes across eight families; optional source .blend copies uniquely matched original textures/previews. Not the original 31 recipes or validated visual parity, no recreated proprietary alphas. Some specialized native presets require visual tuning (including cloth, grain, stamps).

## Verification gates
- CPU: build test-physics-v16, run ctest; new native_material_v32_tests checks zero blend, gain bounds, integrated vector budget, zero-target and opposing-target barriers.
- Blender: same fresh sphere, identical stroke/radius/pressure/texture, stock reference vs MATERIAL (response 0 then .65; transport 0 then .5), compare moved IDs, native target magnitude, visual response, undo, clipping, symmetry, high/low density, p50/p95 time; repeat specialized assets.
- XPBD: only after MATERIAL parity, .10/.20/.35 assist; confirm compliant response and repeat Undo/symmetry fallback tests.
- Warp: from gpu/ run python bench_warp_sculpt_v29.py --device cpu --side 16 --dabs 12 then --device cuda --side 16 --dabs 12 --parity and --device cuda --side 256 --dabs 50 --graph on compatible GPU. Existing Warp compliant XPBD proof does not yet use the Blender editor writer, MultiRes or native target streaming. Verify numerical parity and transfer-inclusive p95 before porting compact backend; no automatic CUDA fallback claims.
- MPM: distinct future volumetric backend with persistent material points, P2G/G2P, stable dt/CFL, constitutive law, contact and conservative surface coupling. Not implemented or validated here. Do not replace the native baseline with it.
No Blender runtime, C++ tests, GPU benchmarks or asset output executed by the author of this commit. All experimental flags opt-in.
