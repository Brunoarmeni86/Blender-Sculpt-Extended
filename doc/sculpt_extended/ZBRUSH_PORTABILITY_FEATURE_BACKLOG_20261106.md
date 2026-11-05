# ZBrush → Sculpt Extended: inventario portabilità e backlog integrato
_Data 2026-11-06 · Stato: ANALISI/ROADMAP (nessuna implementazione implicita)_

Questo documento raccoglie **tutte le funzionalità discusse nella sessione di progettazione**, non soltanto NanoMesh: PolyGroups/Face Sets, modellazione con pennelli, Rake/Roll, hard-surface, Morph, Selection Sets, Polypaint, retopo, MicroDetail, Multires, Vector Displacement e produzione. Collegare le novità con l'infrastruttura già presente in Blender Sculpt Extended, evitando di duplicare funzioni upstream o custom. Include anche la discussione specifica sul pennello NanoMesh e l'Hybrid fuori Sculpt Mode: [progetto dettagliato](NANOMESH_SCULPT_LAYERS_ARCHITECTURE_20261106.md).

**Distinzione stato:** PRESENTE = implementazione individuata (non garanzia di test in tutte le build); PARZIALE = equivalente o componente disponibile, non completa equivalenza ZBrush; PROPOSTO = da progettare/implementare. Difficoltà B/M/A = bassa/media/alta, stima *relativa*; non sono tempi di sviluppo. I prodotti proprietari sono riferimenti di comportamento, non sorgente riutilizzabile.

## A. Modellazione, PolyGroups, NanoMesh e insert

| Feature ZBrush | Situazione Blender / Sculpt Extended | Implementazione o gap | Stima |
|---|---|---|---|
| NanoMesh | GN instancing upstream; nel fork non integrato come sistema | **PROPOSTO** NanoMesh Sculpt Layer + Brush, Instances/VDM/Hybrid, Multires/Mask/Face Sets | M-A |
| PolyGroups | Face Sets upstream + Face Sets HR custom | **PARZIALE** unify workflow, group IDs e selezione bidirezionale; non duplicare dati | B-M |
| Quick PolyGroup | Face Sets painting/selection | Presets rapidi, fill/convert/multi-select e scorciatoie | B |
| Group by Normals | Face Sets + dati normali | Group by angular threshold | B |
| Group by Polypaint | Polypaint HR e Face Sets HR | Clustering/threshold per colore su regioni HR, conversione robusta | B-M |
| Group Loops | Editing mesh upstream | Edge loops lungo boundaries di Face Sets, con garanzie topologiche | M |
| Panel Loops | Inset/Bevel/Solidify upstream | Tool combinato panel extraction + width + thickness + polish | B-M |
| Polish by Groups | Smooth/Plane, Face Sets | Edge-aware smoothing che protegga confini dei gruppi | M |
| ZModeler | Modeling operators nativi in Edit Mode | Workflow contestuale unico, pick target/edge/face e actions | M |
| QMesh | Extrude/Boolean/merge upstream | Extrude/snap/weld contestuali; valutare non duplicazione Edit Mode | M |
| ArrayMesh | Array/GN upstream | Preset di distribuzione interattivo | B |
| MicroMesh di ZBrush | GN instances/mesh conversion | Istanza/sostituzione geometria in render, **non confondere con MicroDetail sparse VDM** | B-M |
| IMM / InsertMesh | Asset Library, mesh assets e GN | Pennello insert/drag + orientamento/scale/simmetria in Sculpt | M |
| Curve IMM | Curve/GN upstream | Stroke→curve→repeated mesh; orientation + spacing + non destructive | M |
| Live Boolean | Boolean modifiers upstream | UX in Sculpt, preview e conferma | B-M |
| ShadowBox-like | Volumetric modeling + remesh upstream | Silhouette multi-view→volume tool | M |

**Decisione esplicita:** per PolyGroups partire da Face Sets HR e Mask già presenti; nessun secondo sistema proprietario di gruppo. Per NanoMesh, vedere specifica separata: **istanze procedurali persistenti**, VDM o Hybrid, brush Paint/Erase/Density/Orient/Scale/Relax basato su Mask/Face Sets; output visibile/valutabile fuori Sculpt Mode.

## B. Pennelli ZBrush-like e workflow stroke

| Pennello/funzione | Base esistente / rischio duplicazione | Gap proposto | Stima |
|---|---|---|---|
| Rake Sculpt Brush | Parametri alpha e stroke Blender | Solchi paralleli e spacing coerenti, preset/asset prima di C++ | B |
| Rake texture orientation | Rake angle della texture già esiste | Orientamento affidabile sulla superficie e durante stroke complessi; distinguere dal Brush Rake | B-M |
| Roll / Ribbon | Texture stroke / surface projection custom, upstream PR in evoluzione | Alpha continuo non stirato, surface-follow, curve, frame tangente parallel-transport | M |
| DamStandard | Crease/Draw Sharp simili | Valutare profilo reale, peak/pinch, pen pressure, falloff, alpha | B |
| HPolish | Plane/Scrape/Multiplane simili | Preset prima, C++ soltanto se differenze misurate | B |
| TrimDynamic | Plane/Scrape simili | Surface clipping/plane falloff orientamento | B |
| ClayBuildup | Clay Strips upstream | Brush asset/preset comportamento | B |
| Slash / Orb Cracks | Crease/Draw Sharp + alphas | Profilo incisioni lungo stroke, texture/alpha | B |
| Chisel / IMM Chisel | VDM Brush e assets upstream | Libreria timbri VDM e curve insert | B |
| Flatten avanzato | Flatten/Plane upstream | Gestione piano e smoothing target | B |
| Move Topological | Topology-based masking e brush filtering | Verificare gap rispetto a ZBrush e Face Set restrictions | B-M |
| Inflate Balloon | Inflate + deform tools | Preset falloff, volume/constraint | B |
| SnakeHook | Upstream | Non duplicare; eventualmente preset | PRESENTE |
| Elastic Grab (Exact/Accelerated) | Presente nel fork | Misurare comportamento e perf con Multires/layers | PARZIALE |
| VDM Draw | Upstream, fork con surface projection | Quality, orientation e compatibilità con layer | PRESENTE/PARZIALE |
| Morph Brush | Snapshot hidden layers non equivalenti a Morph | Ripristino dipinto verso base, layer, snapshot o only-shape/only-detail | M-A |
| Curve Brush e curve stroke | Curve / surface projection V1–V5 | Path stroke con orientation/spacing/radius e Undo singola transazione | M |
| Brush compositi | Multipli brush esistenti | Combo ClayBuildup+Rake, HPolish+Pinch, chaining/recipe e controlli | M |
| Draw/Anchored Surface Transport | Presenti nel sorgente, non universalmente validati | Riutilizzare, non riscrivere; layer/UV/pose/rake interaction | PARZIALE |

**Regola:** iniziare con asset/preset per i pennelli facilmente simulabili e **non aprire nuovi kernel C++ per ogni nome ZBrush**. Per Roll e curve, esplorare il lavoro upstream prima del backport; le versioni della PR possono cambiare. Stabilire test di forma visiva, pressione, angolo, stroke direction, alpha stretch, seams, Multires e layers protetti.

## C. Scultura, deformazioni e Morph/HD

| Feature | Stato / riferimento | Gap e decisione | Priorità |
|---|---|---|---|
| Poseable Symmetry | Frame/pivot/snap e topology pairing già presenti | Per-dab live su geometria posata, cache mapping, Multires samples e Undo | P0 |
| Quick Pose / Transpose | Pose IK e shared-pivot G/R/S migliorati nella release 4 Nov | UX unificata, deformazione pulita e dettaglio layer-aware | P0/P1 |
| Morph Brush / Morph Target | Hidden layer snapshot e Undo non equivalgono a restore point | Brush restore snapshot, Morph to Base/Layer/Snapshot, Shape Only/Detail Only, strength | P1 |
| Sculpt Layers | Strength, reorder/group/solo/merge/commit già presenti | **NON duplicare**; futuro blend modes geometrici e per-layer masks separati | PRESENTE/P2 |
| HD Geometry | Multires e sparse zero-grid | MicroDetail int16/float per tile + edit/viewport residency; non confondere con MicroMesh istanze | P0 |
| Sculptris Pro | Dyntopo upstream | Equivalent solo sul piano concettuale; gestioni diverse | PRESENTE |
| DynaMesh | Voxel Remesh upstream | Fallback/UV detail preservation non equivalenti | PARZIALE |
| ZRemesher | QuadriFlow upstream | Guide e flusso edge loop robusto, solver testing | P2 |
| Project All | Detail Transport Copy custom opt-in | proiezione fra differenti oggetti/topologie con multi-channel data transfer | P2/P3 |
| Extract | HR Face Set/Mask Extract opt-in nel fork | Stabilizzare feature gates, limiti mesh e multi-modifier | PARZIALE |
| ZSphere-like | Skin Modifier e GN | Skeleton organic blockout & adaptive skin workflow | P3 |
| Surface Noise / NoiseMaker | Displacement procedurale + bake→active già presenti | Stack noise, falloff masks, preview, compatibilità multires | P2 |
| Polish by Features | Smooth/Plane già presenti | Edge/feature aware | P2 |
| Decimation Master | Decimate upstream | Controlled projection and re-detail after decimation | PARZIALE |
| History Recall/History Brush | Undo e snapshot presenti | Selective/local historical restore; topology-independent solo dopo | P2/P3 |

La priorità data dall'utente prima di questa sessione resta **Poseable Symmetry → MicroDetail → Adaptive LOD**, con massima attenzione a spikes, cross-level transfer, layer preservation e Undo. NanoMesh può essere prototipato indipendentemente, ma VDM/Hybrid non deve destabilizzare il motore Multires.

## D. Polypaint, Mask, Selection Sets, UV

| Strumento | Stato e nuovo lavoro |
|---|---|
| Polypaint HR | **PRESENTE** con attributi nominati; UV bake solo tile singolo nella versione documentata |
| Polypaint Layers con blend modes | **PROPOSTO** compositing colore (opacity, alpha, Multiply/Screen/Overlay), distinto dai blend mode geometrici |
| Selection Sets | **PROPOSTO** snapshot nominati di Mask/Face Sets/visibility e, in fasi successive, paint, UV, Morph/shape; riuso master già presenti |
| Mask by Cavity / Polypaint / Face Sets | Tool/conversioni Blender parziali; integrare con sorgenti HR e conversioni bidirezionali |
| SpotLight-like texture projection | Projection UI/texture paint + surface-follow; prototipo dedicato |
| UV Master / guided unwrap | UV nativi presenti; aggiungere hint masks/seam guides e densità |
| UDIM Polypaint bake | Current documented UV bake = **one tile**. **PROPOSTO** export multi-UDIM batch con formato corretto e controllo memoria |
| NanoMesh → Mask/Face Sets/Polypaint | **PROPOSTO** reverse mapping delle footprint, per rigonfiamento pelle e binding polypaint |

Selection Sets: non creare copie integrali di ogni grid per ogni stato, preferire snapshot/diff/indici compressi e semantica di livelli; verificare validità dopo modifiche topologiche.

## E. Retopologia, proiezione, export e portabilità

| Funzione | Decisione |
|---|---|
| ZRemesher Guides | Disegnare curve di direzione/loop per guidare retopo (QuadriFlow o solver alternativo) |
| Keep Groups / Keep Creases | Vincoli di confine Face Sets e creases, fail/fallback su topologie impossibili |
| Project All / Projection Shell | Back-projection di geometria, dettagli, layer, colore con limiti distanza/direzione e test |
| Adaptive UV Unwrap | Guidare seam, distorsione e UV density tramite painting |
| Multi-Resolution Reconstruction | Richiede ricerca seria sulla stabilità dei livelli dopo edit base |
| GoZ-like exchange | Interoperabilità di mesh, UV, Polypaint, displacement; **layer geometrici native cross-app non garantiti** |
| NanoMesh output/export | Realize instances, normal/displacement/vector maps e real mesh, con perdita di fedeltà dichiarata quando il campo non può rappresentare i bordi liberi |

## F. Ordine di sviluppo / scope e test

1. **P0 core separato:** Poseable Symmetry live; MicroDetail compatto; Adaptive Sculpt LOD per regioni, tutto in branch indipendenti con regression test.
2. **P1 ergonomia a basso rischio:** PolyGroups via Face Sets HR, quick group, group-by-normal/color, selection conversion; brush assets Rake, DamStandard, HPolish, TrimDynamic, Slash, Orb Cracks, Chisel.
3. **P1 NanoMesh pass A/B/C:** Brush e distribuzione Live/Snapshot, Mask/Face Sets, ancoraggi Multires, aggiornamento Pose; niente modifiche CCG se evitabile; vedi design.
4. **P1 Morph/Selection:** Named selection snapshots, Morph to base/layer/snapshot, detail-only vs shape-only; conservare gli altri layer.
5. **P1/P2 Roll/Surface Brush:** alpha continuum + frame following curvature, curve insertion, stroke Undo; combinazione di brush comportamenti.
6. **P2 NanoMesh VDM/Hybrid/Bake:** field evaluation locale, overlap policy, raccordo organico, viewport Object Mode/render e corrections non distruttive.
7. **P2 advanced:** NoiseMaker stack, Polypaint color compositing, guided QuadRemesh, projection robusta, UV/UDIM pipeline.
8. **P3 research:** topology-independent Morph/History, robust multilayer interchange/GoZ, ZSphere workflows, deep Multires reconstruction.

### Test discriminanti comuni

- Scena organica con braccio posato, maschere e Face Sets HR; stroke alla simmetria su geometria già asimmetrica e salvaguardia degli altri layer.
- MicroDetail/rughe su layer protetto; deformation Pose/Grab al livello basso e ritorno al più alto, verifica payload **e aspetto visivo**; Undo/Redo/save-reopen, niente spike.
- Pennelli confrontati su immagini e mesh identiche (Falloff/Alpha/Pressure/Stroke), oltre a benchmark su superfici dense.
- NanoMesh: 10k–50k scaglie, Edit/Brush/Mask/Face Sets Live e Snapshot, Hybrid con vero bordo libero, update al variare del Multires e con rig Armature.
- Performance: strumenti viewport, CCG/PBVH memory, brush latency, Undo e rebuild. Non trasferire numeri di SculptForge come benchmark del fork.
- Retopo occhi/labbra/arti, UV, ring loops, target game-poly 10k e residual projection.

## Architettura, licenze, precedenti

- ZBrush = comportamenti e riferimenti; nessun riuso di sorgenti/asset proprietari.
- SculptForge = codice open source con licenze da verificare file per file; i suoi algoritmi non sono copiabili 1:1 nel sistema CCG di Blender.
- Il fork usa Multires/SubdivCCG, corner IDs, PBVH, Multires delta layers, lazy color/Face Set masters. Seguire API e snapshot già esistenti.
- Nessuna modifica implicita a build/release; documenti e issues = backlog.
- Evitare nuove sidebar duplicate: mantenere la tab Multires e controlli non ridondanti.

### Riferimenti interni

- [Progetto NanoMesh ibrido, pennelli e Multires](NANOMESH_SCULPT_LAYERS_ARCHITECTURE_20261106.md)
- [Comparazione tecnica SculptForge/ZBrush/forum](COMPARATIVE_AUDIT_SCULPTFORGE_ZBRUSH_FORUMS_20261104.md)
- [Audit funzionalità già presenti](ADDITIONAL_EXISTING_FEATURES_AUDIT_20261104.md)
- [Roadmap Pose/MicroDetail/LOD](ROADMAP_POSE_MICRODETAIL_LOD_20261103.md)
- [Adaptive Sculpt LOD Project](ADAPTIVE_SCULPT_LOD_PROJECT_20261101.md)
- [README build](../../README.md)
