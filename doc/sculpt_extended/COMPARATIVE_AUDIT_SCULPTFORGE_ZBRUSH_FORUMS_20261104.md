# Comparative audit — SculptForge, ZBrush, Blender sculpt forums (4 November 2026)

**Scopo:** scoprire codice realmente riutilizzabile, non duplicare funzioni già sviluppate in Sculpt Extended e distinguere idee ZBrush proprietarie da implementazioni GPL di SculptForge. **Questo documento è una ricerca, non l'implementazione di nuove funzioni.**

**Sorgenti ispezionati staticamente:**
- SculptForge `islandmed/SculptForge`, branch `main`, commit `5f4117b6383a900ad923f9b24cf0f5a38908e5a1`;
- Sculpt Extended branch `checkpoint/adaptive-commit-foundation-20261102`, commit `bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2`, più [inventario precedente](ROADMAP_POSE_MICRODETAIL_LOD_20261103.md) e [audit aggiuntivo](ADDITIONAL_EXISTING_FEATURES_AUDIT_20261104.md).
- Nessuna compilazione/riproduzione benchmark in questa sessione. Non trasferire numeri performance SculptForge a Blender.

## 1. Le scoperte nel codice SculptForge

### A. MicroMesh: height field 16 bit su triangoli — PRIORITÀ ESISTENTE P0
- [Micro.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/Micro.h): `MicroMesh` su triangoli base, livello 1–7, `int16_t` via `MicroStore`, `step` metrico, maschere e dirty bits per triangolo.
- [MicroSculpt.cpp](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/MicroSculpt.cpp): l'area del pennello viene elaborata a risoluzione ridotta e interpolata; `MicroStroke::touch()` acquisisce solo tile modificati, `stitch()` gestisce bordi e duplicati.
- [MicroStore.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/MicroStore.h) e [MicroStore.cpp](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/MicroStore.cpp): untouched tile = tre byte nel formato compresso, bit packing per range, file memory-mapped sparso opzionale; storage out-of-core su POSIX, non presumere identico su Windows.
- [MicroRenderer.cpp](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/render/MicroRenderer.cpp): shader che ricostruisce la microgeometria da posizioni base + displacement (non è una mesh globale di milioni di vertici residenti).
- **Differenza architetturale:** SculptForge campiona patch **triangolari**; il nostro Multires usa CCG **corner grids**, corner identity e coordinate di subdivision. Non copiare indici, seam logic o memory layout senza adattarli. Prototipo consigliato: `int16` per microdettaglio **normale**, opzionale su griglie CCG, con `float` o vettoriale nei casi non rappresentabili. Non ridurre precisione degli attuali Sculpt Layers protetti.

**Limite dell'applicazione di riferimento:** il manuale SculptForge dice che, mentre Micro è attivo, non si possono cambiare normali subdivision levels senza bake/delete. Non replicare questa limitazione: requisito Sculpt Extended è deformazione a livello basso con microdettaglio ancora editabile e non distrutto. https://sculptforge.app/guide/micro-levels.html

### B. FocusDetail: micro streaming + LOD + transazioni — PRIORITÀ ESISTENTE P1
- [FocusDetail.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/FocusDetail.h): commento sorgente **“prototype, off by default”**. File `.sfdetail` compresso, coarse upsampling, promotion per brush e camera con screen error, lookahead, budget di tempo e RAM; conservazione dirty tile in stato parked; save append-only e header atomico.
- `MicroStroke::touch` e Undo obbligano `ensureFull()` prima della scrittura: niente editing su interpolati coarse.
- **Valore architetturale alto**, ma non validato come prodotto universale e non trasferibile 1:1 al PBVH/CCG. Riutilizzare il design del manager/cache e gli invarianti, **non** affermare che Adaptive LOD sia già presente in Sculpt Extended.
- Prototipo proposto: un `RegionResidencyManager` per corner CCG (tile/patch owner + livello di valutazione + dirty generation + budget), in un branch isolato. Niente modifica DNA prima di validazione.

### C. Selection Sets, Morph, permutazione multi-livello — CANDIDATO NUOVO P1
- [SelectionSets.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/SelectionSets.h) e [SelectionSets.cpp](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/SelectionSets.cpp): archivia e ripristina **mask, polygroups, paint, visibility, UV, shape/morph**, per nome e livello. Run-length, `shared_ptr` per snapshot immutabili; union/intersect/subtract; Morph Brush e Morph To Set; persistenza/versionamento/Undo.
- Nella nostra Build esistono già **Face Sets HR, maschere, Polypaint HR, snapshot dei layer**, ma manca questa **composizione di canali selezionabili** con target Morph. Non creare una seconda Face Set storage: usare riferimenti/versioni e mappe master esistenti, con transazioni.
- Stage 1 senza Morph: mask/visibility/Face Sets nominati + operazioni booleane; stage 2 shape snapshots e Morph Brush; stage 3 UV e portabilità interlivello.
- UI semplice in tab Multires, senza duplicare gli operatori nativi.

### D. Color/PBR layers, blend modes — CANDIDATO NUOVO P2
- [PaintLayer.cpp](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/PaintLayer.cpp) e [PaintLayer.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/PaintLayer.h): compositing colore con opacity/alpha e blend modes (Screen/Overlay ecc.).
- **ATTENZIONE:** SculptForge **sculpt geometry layers** = additive per-vertex offsets, non stack di geometry blend modes; i blend mode in `PaintLayer` sono per **colore**.
- Sculpt Extended possiede named Polypaint HR masters, non un compositing stack con opacity/alpha/maschera per colore. Candidato valido, ma verificare compatibilità con il ramo Blender 5.3 texture/attribute display ed evitare di duplicare Ucupaint/Texture Paint.

### E. Brush Recipe e DetailClone — CANDIDATO NUOVO P2
- [BrushRecipe.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/BrushRecipe.h) e [BrushRecipe.cpp](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/BrushRecipe.cpp): brush come ricetta serializzabile di proprietà, non codice eseguibile; pattern procedurali, piani, pinch, drag, cavity, pressure, seed.
- [PathStroke.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/PathStroke.h): stroke riproducibile lungo punti con Catmull-Rom + spacing/deterministic seed, proiezione sulla mesh, Undo come una transazione.
- SculptForge include DetailClone su percorso, mentre Blender contiene già VDM brush e il nostro fork ha Draw Transport e Anchored Surface Projection. **Gap:** libreria di dettaglio/clonazione VDM ripetuta e orientata lungo uno stroke con superficie deformata; per noi conviene integrare con Brush Asset/Stroke API esistente, non sostituire tutto il motore.

### F. Quad Remesh Guides — CANDIDATO NUOVO P2/P3
- [QRemeshGuides.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/QRemeshGuides.h) e [QRemeshGuides.cpp](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/QRemeshGuides.cpp), [QRemesh2Field.cpp](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/QRemesh2Field.cpp): guide disegnate sulla superficie che influenzano campo direzionale e possono vincolare il loop; la documentazione avverte che curve chiuse e attraversamento della simmetria hanno percorsi differenti.
- **Sculpt Extended non ha un sistema nativo con identiche garanzie** nel codice ispezionato; Blender ha QuadriFlow di base e l'utente ha un progetto separato di miglioramento QuadriFlow. Preferire prototipo **indipendente** e integrazione nella retopo, non toccare CCG/Undo stabile prima di avere solver affidabile.
- Test: anelli occhi/labbra, curva occhio chiusa, guida su simmetria, coste rigide, target 10k per game, residual reprojection.

### G. Feedback reattivo e ottimizzazione periferica — CANDIDATO P2
- [TierConfig.h](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/src/core/TierConfig.h): budget triangoli attivi, GPU upload e shade per frame, target FPS, profilo RAM/VRAM/battery.
- [docs/guide/performance.md](https://github.com/islandmed/SculptForge/blob/5f4117b6383a900ad923f9b24cf0f5a38908e5a1/docs/guide/performance.md): fast navigation usa una rappresentazione alleggerita *soltanto durante l'interazione*, predictive stroke renderizza anteprima, draft normal/cavity update differito. I numeri riportati sono benchmark di SculptForge su hardware indicato, non confronti con il nostro.
- Possibile quick win: renderer viewport-only proxy durante orbit + budget GPU uploads + differimento normali quando l'operazione è in corso (con validazione contro bounding/raycast), senza modificare la geometria persistente.

## 2. ZBrush: priorità come comportamento, non codice copiabile

| Caratteristica ZBrush | Base già presente nel fork | Gap specifico | Difficoltà |
|---|---|---|---|
| Poseable Symmetry | frame persistente, topology pairing, Pose, symmetry passes | mapping rest→posed live per every dab e Multires sample | ALTA |
| Morph Target / History Brush | copia hidden layer e Undo Delta | ripristino locale selezionato/History region con Undo multi-livello, opzionalmente projection topology-independent | MEDIA–ALTA |
| ZRemesher Guides | retopo Blender/QuadriFlow; esperienza con guide separate | field-aligned loops e ritorno dettaglio/layer | ALTA |
| Surface Noise / NoiseMaker | texture displacement procedurale e bake→active | stack di noise/falloff/masks, manipolazione alpha UV/surface, visual preview; **non** rifare displacement | MEDIA |
| IMM / NanoMesh / curve insert | Blender asset/tool GN/collection instances | rapido stamp o curve placement in Sculpt Mode, export non distruttivo | MEDIA |
| HD Geometry | Multires e nostri sparse layer | compact microdisplacement fuori dalla CCG completa | ALTA |
| Transpose / Proxy Pose | Sculpt GRS, pivot condiviso, Pose | UX posa integrata e dettaglio trasportato, niente cambio Mode | MEDIA–ALTA |
| 3D Layers e gruppi | Già in fork (additive, strength, group, reorder, merge, bake) | NON rifare; considerare, separatamente, geometry blend modes e mask per layer | MEDIA–ALTA |
| Selection Sets | Face Sets/mask/Polypaint master presenti | salvare/ripristinare set nominati multi-canale e multi-livello | MEDIA |

Riferimenti ufficiali:
- Poseable Symmetry https://help.maxon.net/zbr/en-us/Content/html/user-guide/3d-modeling/sculpting/symmetry/symmetry.html
- History Brush/IMM e Curve tools https://www.maxon.net/en/zbrush/features/brush-system
- NoiseMaker https://help.maxon.net/zbr/en-us/Content/html/user-guide/3d-modeling/sculpting/surface-noise/surface-noise.html
- Selection Sets / release recenti https://help.maxon.net/zbr/en-us/Content/html/features/whats-new-in-zbrush/zbrush-whats-new.html

## 3. Comunità Blender: esigenze emerse (segnali qualitativi, NON percentuali di utenti)

1. **Alpha roll / rake / ribbon continuo**: BlenderArtists aprile 2026 segnala mancanza Slash2 e distorsione rake; Right-Click Select chiede deformazione dell'alpha along stroke. Un PR Blender roll curve #155072 è ancora descritto WIP: confrontarlo prima di fare port/rifacimento. Questa funzione NON coincide con la nostra proiezione Anchored (piatta/di percorso) e deve poter lavorare su **stroke libero e curve**.
2. **Responsiveness con mesh dense**: BlenderArtists ottobre 2026 segnala blocchi autosave e che nascondere parte della mesh migliora poco le brush. Non promettere “hide = boost” nella Build. Proporre profili e benchmark che separano brush calculation, CCG evaluation, PBVH, upload, save/undo.
3. **Retopologia automatica**: discussione r/blender marzo 2026 sull'insufficienza di QuadriFlow in forme complesse. Guide e solver affidabile contano più di nuovi preset.
4. **Ergonomia e accesso immediato**: opinioni contrastanti: alcuni artisti professionisti lavorano più velocemente in Blender dopo aver personalizzato gli strumenti; altri segnalano ancora frizioni prestazionali. Non trasformare ogni elemento ZBrush in clone dell'interfaccia; preservare UX Blender nativa.
5. **Non duplicare novità upstream**: Blender 5.3 aggiunge visualizzazione attributi e texture in Multires, supporto curva pressione e tip roundness si stanno estendendo in upstream; rake e roll sono in lavorazione/PR. Verificare commit esatto prima dei backport.

Fonti community:
- https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=266
- https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=274
- https://blenderartists.org/t/the-big-blender-sculpt-mode-thread-part-2/1395490?page=252
- https://blender.community/c/rightclickselect/YyXp/
- https://www.reddit.com/r/blender/comments/1s6mw4r/sculpting_in_blender_as_a_zbrush_user/
- https://www.reddit.com/r/blender/comments/1t1yl0c/blenders_sculpting_as_good_as_zbrush_wip/
- https://projects.blender.org/blender/blender/pulls/155072
- https://devtalk.blender.org/t/2026-10-27-sculpt-paint-texture-module-meeting/46228
- https://developer.blender.org/docs/release_notes/5.3/sculpt/

## 4. Ordine di sviluppo raccomandato e non duplicazione

### Confermare le priorità già concordate
- **P0 A — Poseable Symmetry:** mapping già presente → stroke live + trasporto visuale dei dettagli nelle pose → Undo/per-layer invariance.
- **P0 B — MicroDetail:** prototipo `int16` patches su CCG, conservazione del dettaglio nel deformare low levels, confronto float vs quantized, nessun obbligo di bake per cambiare livello.
- **P1 C — Adaptive LOD:** sperimentare FocusDetail-like manager e render cache dopo la micro rappresentazione. *Non* assumere che esista LOD misto già nella Build.

### Incrementi collaterali fattibili, senza perdere focus
1. **P1 quick win sperimentale**: HPolish/DamStandard/Slash alpha test su hard-surface organico; controllare upstream tip roundness/pressure.
2. **P1 subito dopo Pose V1:** Selection Set *soltanto* mask+Face Sets+visibility (senza nuova geometria) → Morph come fase successiva.
3. **P1/P2:** ribbon Roll/Surface-follow: progettare frame di superficie lungo il percorso, VDM/alpha clone; non riscrivere Anchored V1–V5. Basare prototipo sulla nostra Surface Projection e su spline/brush engine.
4. **P2:** viewport fast-navigation e profili budget GPU/normal, comparazione OFF/ON; evitare regressioni sulla fluidità.
5. **P2:** Polypaint layer stack colori + blend; NON geometry layer blend confuso con colore.
6. **P2/P3:** Quad Remesh Guides separato; IMM/Sculpt Multi-Object via integrazione UX Blender e possibili addon.
7. **P3:** History Brush topology-independent e import/export interoperabile multilayer: richiede nuove regole di proiezione, differenze tra topologie e test robusti.

## 5. Gate di qualità e vincoli

- Stabilità prioritaria: spikes, layer preservation, L0, mid-Multires, coordinate seams, Undo/Redo, save/reopen. Ogni performance report contiene dati effettivi macchina/modello/commit.
- **Isolamento dati layer ≠ preservazione visiva dettaglio**: con Grab/Pose/Elastic bisogna trasportare il campo di dettaglio, non compensarlo solamente nell'attivo.
- SculptForge è principalmente **GPL-3.0-or-later**; Blender sviluppato con header prevalentemente **GPL-2.0-or-later**. Il riuso diretto di sorgenti GPL-3.0-or-later in un'opera combinata è normalmente possibile scegliendo GPL-3.0-or-later per la combinazione, ma richiede revisione della licenza file-per-file e delle dipendenze, preservare copyright/attribution e distribuire sorgente conforme. Vedi https://www.gnu.org/licenses/license-compatibility.en.html e https://www.blender.org/about/license/ . Non copiare codice, UI asset o dati proprietari ZBrush: analizzare soltanto idee e comportamenti.
- Architettura differente: SculptForge usa triangoli GL ed un proprio scene/render/undo system; Blender usa OpenSubdiv, CCG, PBVH, DNA/RNA, undo nativo e viewport engines. “Portabile” spesso significa portare **algoritmo o strategia**, non compilare lo stesso file in Blender.
- I prototipi non diventano P0 integrati prima di tests su Windows build stable e Linux separata.

## 6. File e punti di ingresso del nostro fork

- [symmetry_shape_v2.py](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/scripts/startup/sculpt_extended_integrated/symmetry_shape_v2.py)
- [sculpt_pose.cc](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_pose.cc)
- [sculpt.cc](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt.cc)
- [multires_sculpt_layers.cc](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/blenkernel/intern/multires_sculpt_layers.cc)
- [sculpt_layer_detail_v01.hh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_layer_detail_v01.hh)
- [sculpt_undo.cc](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc)
- [sculpt_polypaint_v04.hh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_polypaint_v04.hh)
- [sculpt_multires_hr_extract_v05.hh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_multires_hr_extract_v05.hh)
- [DNA_modifier_types.h](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/makesdna/DNA_modifier_types.h)

**Conclusione:** priorità di performance più promettente è un manager di microtile con policy LOD/streaming, ma il prototipo più urgente resta Poseable Symmetry sul sistema già esistente. Per funzionalità nuove ad alta utilità con rischio più basso: Selection Sets/Morph e Stroke Rake/Roll, purché non si ripeta codice Blender upstream.
