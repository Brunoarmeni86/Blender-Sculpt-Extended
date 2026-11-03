# Sculpt Extended — roadmap corretta dopo audit del codice (3 novembre 2026)

**Priorità confermate:** Poseable Symmetry/Quick Pose → MicroDetail → Adaptive Sculpt LOD.
**Correzione alla prima annotazione:** molte capacità considerate da implementare **sono già presenti**; non riscriverle né ricominciare da zero.

## Quale sorgente è stato controllato

- Sorgente **integrato stabile**: `checkpoint/stable-20261027` @ `1eaaa7d9fe8b`. La documentazione può riferirsi a release/tag differenti.
- Sorgente di lavoro più recente esaminato: `checkpoint/adaptive-commit-foundation-20261102` @ `bda4dcbf0e99` (tracker aggiornato anche il 3 novembre).
- **Checkpoint Windows approvato** del 28 ottobre: `233b1a2da926`. Non confondere la presenza nel sorgente successivo con funzione testata e distribuita in quel binario.
- `main` è principalmente documentazione/source meno integrato: per l'audit funzionale controllare i branch sopra, non dedurre assenze dal solo `main`.
- L'audit è **statico** su file sorgente e documenti/test registrati nel repo: non equivale a esecuzione del build più recente su Windows/Linux.

### Legenda

- **PRESENTE:** codice/operatori e API effettivamente presenti nel sorgente indicato.
- **PARZIALE / SPERIMENTALE:** componenti presenti, ma non dimostrano il comportamento finale richiesto o richiedono un flag/test.
- **NON PRESENTE COME SISTEMA:** non identificata un'implementazione integrata della caratteristica completa nei componenti ispezionati; non significa che ogni sua subfunzione sia assente.
- **UPSTREAM:** funzione di Blender già esistente, non invenzione del fork.

## Inventario verificato e gap reali

| Area | Stato nel nostro sorgente | Cosa riutilizzare | Che cosa manca davvero |
|---|---|---|---|
| Simmetria con riferimento orientabile | **PRESENTE** | Frame persistente, assi spostabili e uso nei pass dei pennelli; G/R/S per frame | Mapping topologico continuo su mesh posata |
| Pivot Sculpt condiviso/indipendente | **PRESENTE** | Pivot metadata-only, snap, gizmo; niente commit per muovere solo il pivot | UX Quick Pose unificata senza duplicare il pivot |
| Transfer topologico della forma | **PRESENTE** | `topology_pairs`, `topology_sides`, trasferimento con maschera | Riutilizzare/portare mapping in un cache nativo di per-stroke symmetry; non usare un BMesh matching a ogni dab |
| Symmetrize Multires Active Detail | **PRESENTE, VALIDAZIONE SPERIMENTALE** | `symmetrize_active_detail`, coppie reciproche ottimizzate | Simmetria **durante ogni stroke** su geometria asimmetricamente posata |
| Pose Brush, IK, elastic deformation | **PRESENTE (anche UPSTREAM)** | Pose chain, falloff e IK; Elastic Exact/Accelerated custom | Quick Pose con Bend/Twist/falloff condiviso, return-to-brush e layer transaction robusta |
| Sculpt Layers: reorder/group/merge/solo | **PRESENTE** | API `move`, `group_create_from_active`, `group_merge`, `group_toggle_visibility`, `merge_down`, UI sidebar | Solo rifiniture, test e UX, non nuovo stack |
| Sparse layer displacement | **PRESENTE** | `grid_ensure`: payload nullo = zero implicito; `payload_grids` cache, compattazione legacy al caricamento | Compressione per tile non-null, eventuale paginazione/residency: design distinto |
| Polypaint HR / Face Sets HR | **PRESENTE** | Master indipendente, lazy grid, proiezione locale colore; Face Sets a risoluzione elevata | Verificare interazione con microdetail/LOD, non riscrivere polypaint |
| Multires high-detail | **PRESENTE (UPSTREAM + fork)** | Displacement CCG e Sculpt Layers, detail transport | Un **MicroDetail dedicato** con storage/valutazione a risoluzione molto superiore senza CCG globale equivalente |
| Local Commit e aggiornamento di aree interessate | **PRESENTE, CASI FALLBACK** | Journal griglie dirty, CCG/PBVH preservati quando possibile; selezione sparse facce per normali | Gestione di **risoluzioni miste nello stesso oggetto**, halo e PBVH variabile |
| Multires levels / viewport/render levels | **PRESENTE (UPSTREAM)** | LOD globale preesistente, differenza render/sculpt | LOD **per regione/griglia** e auto-promozione sotto pennello |
| Adaptive Sculpt LOD | **PROGETTO DETTAGLIATO, NON IMPLEMENTATO** | [Specifiche LOD del 1 novembre](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/1eaaa7d9fe8b7514edff6979dabebefa835e5202/doc/sculpt_extended/ADAPTIVE_SCULPT_LOD_PROJECT_20261101.md) | Fase A–D: profili, lock regioni, mixed-resolution SubdivCCG, transizioni crack-free, local PBVH |
| Anchored Surface Projection/Transport | **PRESENTE nel sorgente, OPT-IN/SPERIMENTALE** | V1 curvature-adaptive, V2 normal-guided, V3 surface-path, V4 tangent transport; V5 path nelle routine recenti | Test visivo, simmetria mirror/radial/tiled e compatibilità con microdetail/LOD; non ricreare sistema |
| Vector Displacement Brush, Elastic Grab | **PRESENTE** | `draw_vector_displacement.cc`, `elastic_grab_exact.hh`, `elastic_grab_level_one.hh` | Ottimizzazione e compatibilità layer in casi avanzati, non nuova brush architecture |

### Fonti di codice principali

**Simmetria e posa:** [topology_pairs() e transfer()](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/scripts/startup/sculpt_extended_integrated/symmetry_shape_v2.py); [operatore Multires detail](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/scripts/startup/sculpt_extended_integrated/symmetry_multires.py); [pivot e snap](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/scripts/startup/sculpt_extended_integrated/reference_native_snap.py); [Pose Brush/IK/reference](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_pose.cc); [symmetrize_active_detail e API layer](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/blenkernel/BKE_multires_sculpt_layers.hh).

**Layer/micro base:** [sparse payload e commit](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/blenkernel/intern/multires_sculpt_layers.cc); [group, move, polypaint HR, grid_ensure](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/blenkernel/BKE_multires_sculpt_layers.hh); [UI layer e gruppi](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/scripts/startup/bl_ui/space_view3d_multires_sidebar.py); [Preserve Other Layers su CCG](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_layer_detail_v01.hh).

**Proiezione:** [opzioni Anchored Surface V1–V4](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/1eaaa7d9fe8b7514edff6979dabebefa835e5202/source/blender/makesrna/intern/rna_brush.cc); [V5 path e fallbacks](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt.cc); [proiezione locale del polypaint (distinta dalla Surface Projection)](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/1eaaa7d9fe8b7514edff6979dabebefa835e5202/doc/sculpt_extended/POLYPAINT_LOCAL_PROJECTION_20261022.md). Non confondere `sculpt_project.cc`/Scene Project con l'alpha Surface Projection, né il pass di colore HR con la proiezione di texture per i pennelli.

**Stato e prove:** [tracker performance Nov 2–3](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md); [checkpoint approvato 28 ottobre](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/233b1a2da926d9200da5321674e54038875813b1/doc/sculpt_extended/CHECKPOINT_20261028.md).

## PRIORITÀ 1 — Poseable Symmetry + Quick Pose [#6](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/6)

**Non sviluppare di nuovo:** frame di riferimento, snap/pivot, Shape Only topologico, operatore di simmetrizzazione Multires, Pose Brush/IK, G/R/S, Elastic Grab.

**Sviluppare:** collegare le corrispondenze vertice↔vertice/rest pose a **ogni dab** in Sculpt Mode sulla mesh deformata e risolvere una corrispondenza per sample Multires, gestendo self-mapped, maschere, Face Sets, regioni con simmetria non geometrica, layer protetti, Undo e seam. Mantenere mappa persistente con invalidazione topologica. Integrare un Quick Pose molto semplice attorno ai controlli di posa/pivot già presenti. Nuovi falloff geodesici comuni a Pose/G/R/S sono **proposta**, come esplicitato nel tracker, non una funzione già validata.

**Gate:** braccia posate diversamente, stroke su una braccia con replica topologica corretta sull'altra; salvataggio/Undo/Redo e layer non attivi invariati.

## PRIORITÀ 2 — MicroDetail [#7](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/7)

**Non sviluppare di nuovo:** livelli Multires, sparse zero-grid displacement di Sculpt Layers, local Commit, master polypaint/Face Sets, Draw Vector Displacement.

**Sviluppare:** livello MicroDetail sopra/accanto alle griglie Multires con rappresentazione per tile/microgrid compressa, residenza adattiva, dettaglio che segue i cambiamenti low-level, coordinate e frame con seam ownership, Undo diff e bake. Il semplice fatto che oggi alcune griglie layer siano implicite non significa che esista già un micromesh renderer o un microtile storage per dettaglio non-zero.

**Gate:** pori/rughe su layer micro dopo deformazione di volumi bassi, nessun drift/spike; confronti RAM, latency e visibilità a stessa precisione.

## PRIORITÀ 3 — Adaptive Sculpt LOD [#8](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/8)

**Non sviluppare di nuovo:** suddivisioni uniformi Multires e renderer levels, local Commit, PBVH per node, sparse face selection, sistemi Face Set/mask già presenti.

**Continuare le specifiche già esistenti:** [ADAPTIVE_SCULPT_LOD_PROJECT_20261101.md](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/1eaaa7d9fe8b7514edff6979dabebefa835e5202/doc/sculpt_extended/ADAPTIVE_SCULPT_LOD_PROJECT_20261101.md). È già scritto un progetto completo (profili nominati, regioni Face Set/maschera, lock minimo/esatto, brush-promotion, CCG mixed-level, seam ownership e quadtree); lo stato nel documento stesso è esplicitamente **NOT IMPLEMENTED**. Non creare una seconda architettura parallela.

**Gate:** due aree dello stesso oggetto con livello reale di valutazione differente, transizioni crack-free, local brush promotion e master detail invariato.

## Surface Projection — presente, non da duplicare

Smentita la precedente dicitura *“solo dichiarata dall'utente, non verificata nel codice”*: nel sorgente integrato stabile esistono le opzioni RNA `use_anchor_surface_projection`, `use_anchor_normal_guided_projection`, `use_anchor_geodesic_projection`, `use_anchor_surface_transport`; il percorso di sampling e fallback si trova in `sculpt.cc`, e l'evoluzione V5 compare nel ramo di lavoro. **Codice presente ≠ comportamento universalmente convalidato**, specialmente per mirror/radial/tiled e in presenza di grandi stamp.

## Regole

- Priorità richieste confermate, ma ogni sviluppo parte dalla **componente già presente**, non da una riscrittura.
- Prima di ogni merge aggiornare baseline/commit e confrontare branch stabile, approvato e sperimentale.
- P0 trasversale: evitare regressioni degli spikes e dell'Undo e rispettare il checkpoint Windows approvato; Linux certificato separatamente.
- Aprire PR o branch esperimenti distinti, con opt-in, test su mesh sintetica e scene reali, confronti OFF/ON.
- Non confondere **codice presente**, **flag opt-in**, **test registrato** e **funzione validata nella Build distribuita**.


## Funzioni della comparazione ZBrush/SculptForge fuori dalle tre priorità

Queste voci erano state prospettate come *nuove* nel primo confronto; sono riclassificate per non generare backlog duplicato:

| Voce originaria | Classificazione | Nota |
|---|---|---|
| VDM / Vector Displacement brush | **PRESENTE (UPSTREAM)** | `source/blender/editors/sculpt_paint/mesh/brushes/draw_vector_displacement.cc`. Eventuali miglioramenti non sono una nuova implementazione del brush. |
| Elastic Grab e Twist | **PRESENTE + OTTIMIZZAZIONI CUSTOM** | `brushes/elastic_deform.cc` seleziona `BRUSH_SCULPT_ELASTIC_EXACT` o `ACCELERATED` (opt-in); usa `elastic_grab_exact.hh` e `elastic_grab_level_one.hh`. |
| DamStandard / hPolish | **STRUMENTI ANALOGHI PRESENTI, NON EQUIVALENZA VALIDATA** | Crease, Draw Sharp, Plane, Multiplane Scrape sono già in `brushes/`. Un nuovo brush va proposto solo dopo confronto del comportamento con gli asset/parametri esistenti. |
| Selection Sets / Morph Brush | **CAPACITÀ PARZIALI** | Face Sets, maschere, snapshot e layer nascosti esistono. Non verificato uno stack multicanale di Selection Sets e Morph-to-saved-shape come quello di SculptForge; progetto separato a bassa priorità. |
| Paint Layers con Blend Modes | **NON EQUIVALENTE AL POLYPAINT HR** | Ci sono Color Attributes nominati e master HR indipendenti, ma non è stata verificata una composizione completa di layer colore (Overlay/Screen/Multiply) nel sistema nativo Multires. |
| Quad Remesh guidato | **QUADRIFLOW PRESENTE (UPSTREAM), GUIDE NON VERIFICATE** | `source/blender/editors/object/object_remesh.cc` contiene QuadriFlow; guide di flusso e preservation avanzata restano eventuali estensioni, non un nuovo remesher. |

**Nota:** “non verificato” non significa prova di assenza nell'intero repository. Per promuovere una voce a implementazione nuova sono necessari grep completo, conferma delle API e un test operativo.
