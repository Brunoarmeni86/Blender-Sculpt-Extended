# Sculpt Extended — Multires Sculpt Layers VNext: ricerca e piano di esperimenti
Data: 2026-11-12. Stato: RESEARCH / DESIGN — nessuna patch, build o benchmark eseguiti da questo memo.
Baseline da preservare: tag v5.2.3-sculpt-extended-20261104, SHA 09a16d4b8482200101cb30728881ca4524edf019. Branch di ricerca distinto dalla build stabile e dalla migrazione 5.4.
Documento complementare: [BMAX_CURIO_REMESH_RESEARCH_20261112.md](BMAX_CURIO_REMESH_RESEARCH_20261112.md).

## 1. Obiettivo e vincoli
Migliorare latenza UI, Undo/Polypaint, sicurezza delle transazioni, RAM/VRAM, regolazione Strength e trasporto del dettaglio delle griglie Multires. **Non riscrivere ciò che funziona**: griglie sparse a delta-zero implicito, Commit LOCAL, dirty-grid/sample journal, frame/tangent cache, incremental Weight/Visibility, First Stroke e Undo same/cross-level, Face Sets/Polypaint HR e fallback uniformi esistono. La priorità funzionale resta Poseable Symmetry + Quick Rig (#6).

Per Blender 5.4: mantenere modello dati e codice facilmente isolabile; non investire ora in una sostituzione completa di SubdivCCG/PBVH sul fork 5.2.3. Le modifiche al renderer PBVH 5.4 non risolvono da sole i costi di composizione CPU.

## 2. Audit del codice della release (SOURCE-CONFIRMED)
- source/blender/blenkernel/intern/multires_sculpt_layers.cc: payload_grids(), indice cache delle griglie occupate, sparse zero, Polypaint/FaceSet master.
- source/blender/blenkernel/intern/multires.cc: multires_sculpt_layer_commit_top_level_local(), ChangeSet, frame cache, solve/publish parallel, weight_direct_delta(), quota/cache RAM e fallback GLOBAL.
- source/blender/makesrna/intern/rna_modifier.cc: rna_MultiresSculptLayer_strength_set(), rna_multires_visible_fast_refresh(), rna_MultiresModifier_active_layer_set().
- source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc: snapshot/payload swap del layer, restore cross-level, native Undo.
- source/blender/editors/sculpt_paint/mesh/sculpt.cc: sculpt_polypaint_prepare_stroke_master() e checkpoint Memfile di preparazione.

**Correzione al vecchio backlog:** il setter Strength e la selezione layer saltano già la chiamata a Commit quando il SubdivCCG non ha coordinate pendenti (dirty.coords=false). Anche la selezione metadata-only esiste. Non implementare nuovamente P1/P3 del vecchio tracker senza controllare la release, né chiamare genericamente slow una funzionalità già veloce.

Funzioni opt-in non validate da non promuovere automaticamente: BLENDER_SCULPT_LAYER_REGION_V01, BLENDER_SCULPT_WEIGHT_MULTI_CACHE, BLENDER_SCULPT_WEIGHT_HOT_SKIP_STITCH; le prove precedenti di progressive cold cache, compattazione in stroke e migrazione legacy in percorso hot hanno registrato regressioni.

## 3. Evidenze storiche (NON benchmark di questa ricerca)
Fonte: doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md e Preservation Contract 5.4.
| Caso | Numero storico | Interpretazione |
| --- | ---: | --- |
| Selezione layer dopo superficie pulita | mediana 0.0018 ms | già metadata-only |
| Strength layer denso, 122.67 M campioni | circa 3.2 s | delta circa 1.9 s; normali/stitch circa 0.7 s; PBVH bounds circa 0.64 s |
| Strength layer sparso caldo | 82-96 ms | utile indice sparse, non rappresentativo di layer denso |
| Commit LOCAL dopo G/R/S | 395.439 ms | non confondere con la pausa successiva Undo/Polypaint |
| Commit 52.77 M campioni | solve 915 ms, publish 1058 ms | publish costa quanto solve |
| Preparazione Polypaint e Memfile | Undo fino a 58.3 s; rebuild CCG 9.8-13.3 s | priorità latenza/affidabilità |
| RAM in un test di budget | working circa 15 vs 23 GB | prova circoscritta |

**Non inferire speedup nuovi.** Rieseguire su binario release 4 novembre e confrontare cold/hot, p50/p95, picchi e motivo fallback.

## 4. P0 — Undo/Polypaint: eliminare checkpoint Memfile ripetuti
Osservazione dal sorgente: la preparazione di Attribute e Resolution usa più ED_undo_push (Before Polypaint Attribute Change, Prepare Polypaint Attribute, Before Polypaint Resolution Change, Prepare Polypaint Resolution), con costo elevato di decode e rebuild CCG in specifici workflow.

Proposta: introdurre un contratto PaintPreparationTransaction con fasi inspect > stage > validate > publish > undo-boundary. Sostituire coppie di checkpoint quando si può dimostrare che un unico passo Undo nativo rappresenta correttamente le modifiche ai payload colore e i metadati; non fare ricalcoli globali intermedi. Conservare il master originale finché la transazione non passa; nessuna modifica se la preparazione fallisce.

**Alternative da testare (nell'ordine):**
A) accorpare gli attuali Memfile in una sola boundary (minor rischio);
B) step dedicato ColorMasterSwap con scambio dei buffer, più snapshot dei metadati;
C) copy-on-write per griglia e attributo e compressione solo delle revisioni fredde.
Mai introdurre un nuovo Undo che non interagisca con le operazioni di Blender e con saved .blend.

Test: primo Paint e cambio Attribute, Float/Byte e Point/Corner, L0 e livello massimo, Color Filter/Blur/Smear, Esc, Undo/Redo attraverso cambi di livello, rebuild CCG, append/duplicate, save/reopen; layer hidden invariati.

## 5. P0 — Commit transazionale e failure injection
Osservazione dal sorgente: solve e validazione precedono publish_range; tuttavia nel publish vengono ancora allocati i payload (grid_ensure_deferred_cache_invalidation) e alcune griglie possono essere già scritte quando una successiva fallisce. publish_ok viene verificato dopo le scritture. **Questo è un rischio da riprodurre con fault injection, non una corruzione già dimostrata.**

Architettura ipotizzata: LayerGridTransaction.
1. Capture ChangeSet e generation/owner del layer.
2. Preflight: tutte le nuove allocazioni e lo staging, controlli di shape, finitezza, mapping e RAM budget.
3. Solve su dati isolati; validate completo prima di una scrittura ai payload di destinazione.
4. Publish senza allocazioni fallibili; se necessario usare replacement buffer + swap, o rollback journal delle sole dirty grids.
5. Una sola cache invalidation, one-step Undo, notify/Depsgraph/PBVH solo dopo il successo.
6. Fail/cancel = dati, layer nascosti e Undo identici al prima.

Test allocator failure sulla prima/metà/ultima griglia, NaN, mismatch topology, concorrenti in thread, diverso ordine dei chunk, salvataggio dopo errore, hidden layers bytewise invariati. Misurare overhead del preflight e memoria extra, nessuna full copy di 100M campioni.

## 6. P1 — Strength Preview interattivo sui layer densi
Non proporre nuovamente uno slider solo differito: l'utente ha già preferito l'anteprima visiva. Confrontare tre strategie:
A) coalescing degli eventi UI senza qualità ridotta, sempre exact a ogni update effettivamente presentato;
B) anteprima temporanea valutata su un livello CCG inferiore/proxy, mantenendo master HR non toccato;
C) valutazione preview ROI dei nodi visibili o vicini al cursore, full refresh sul release.

Begin: commit solo se pending, snapshot del valore e del layer generation, una transazione Undo. Update: non accumulare errori, derivare sempre dal valore begin, accorpare eventi senza perdere input finale, non esportare/renderizzare la preview approssimata. End: valutazione identica al percorso attuale sull'intero supporto, stitching/normals/bounds coerenti e una sola operazione Undo. Cancel: rollback esatto. Fallback automatico al percorso corrente quando preview non valida.

Misure distinte di latency preview p50/p95, costo del rilascio (finalize), popping e errore visivo; su layer 100% la preview può migliorare il drag ma NON elimina necessariamente i circa 3.2 s del passaggio esatto finale.

## 7. P1 — support index, PBVH, staging
- Esiste un indice runtime layer -> griglie occupate, attualmente concentrato sul layer hot. Verificare se conviene una tabella inversa grid -> PBVH node e una cache multi-layer **con budget**, revision e invalida dopo topology, level, PBVH rebuild, Undo, duplicate e modifiche a payload.
- Nel setter rna_multires_visible_fast_refresh viene creato affected_grids e sono scanditi i nodi PBVH per identificare quelli intersecati; un indice inverso potrebbe ridurre il costo per layer sparsi. Misurare setup/lookup e RAM.
- Il limite di bypass locale dirty_grids > 50% è euristico. Dopo un profiler dettagliato provare cost model basato su work samples + memory pressure + piano normals/bounds; qualunque cambio deve essere deterministico e preservare il fallback.
- Nessuna cache float3 completa di 122.67 M campioni: richiederebbe circa 1.4 GiB, fuori dall'attuale budget (256 MiB nel test).
- Differenziare dirty coords/normals/mask/color/facesets/bounds e notifiche del Draw Manager. Profilare lock contention, buffer packing, false sharing ed eventuali scansioni globali. Non assumere che ulteriore multithreading migliori: Blender GSoC 2025 non ha misurato benefici nei tentativi intra-nodo.

## 8. P1 — Servizio Detail Transport unico
In multires.cc la correzione low->high già converte i delta tangent-space in object-space, interpola e ricodifica nel frame locale di ciascun sample fine per evitare spikes. Non duplicare il metodo.

Nuovo contratto: DetailTransportService con operazioni separate:
- transportExistingDetail(base deformation, frame transport, identity): i dettagli dei layer non attivi seguono una grande posa senza alterare i payload master;
- recordNewDelta(activeLayer, original/evaluated brush sample): scrive solo sul layer attivo, rispettando maschere, visibility e Undo.

Riutilizzare per Pose Brush, S/R/T, Grab, Armature/Quick Rig e Poseable Symmetry. Gestire frame invertiti/degeneri, forti curvature, grandi rotazioni, dual quaternion, seam e cambio Multires livello. Per trasformazioni non lineari dichiarare fallback e precisione. Test di posa asimmetrica arti, corna sottili, dettaglio inciso prima della deformazione, ritorno L0->max, hidden layer bitwise stabile.

## 9. Nuove piste emerse dalla ricerca online
1. **Blender Sculpt Mode Refactor 2024:** struct-of-arrays e dirty flags per attributo; la refactor documenta notevoli benefici nel draw e riduzione di copie inutili. Non reintrodurre refresh di tutti i buffer dopo un edit di sole maschere/colore. https://code.blender.org/2024/11/this-summers-sculpt-mode-refactor/
2. **GSoC 2025:** mesh spatial reorder, Undo sparse compression/lazy decompression. Il report cita 20-40% di risparmio Undo in alcune prove; prima verificare se è integrato nel commit upstream mirato e adattarlo, non duplicarlo. https://devtalk.blender.org/t/gsoc-2025-optimizing-mesh-and-multires-sculpting-performance-in-blender/40368/1
3. **OpenSubdiv:** PatchTable/LimitStencilTable per valutare facce o campioni selezionati; possibile esperimento di batch dei tangent-frame nelle dirty regions, non sostituto comprovato del Multires displacement. https://github.com/PixarAnimationStudios/OpenSubdiv/blob/dev/documentation/tutorials.rst e https://opensubdiv.org/docs/doxy_html/a01008.html
4. **SculptForge Performance:** profiler per dab/normal/BVH/GPU upload e frame-budget governor. Idea trasferibile: piccolo pannello Sculpt Extended Profiler e log on-demand, non una dipendenza esterna. https://sculptforge.app/guide/performance.html
5. **SculptForge Micro Levels:** height-only 16-bit + interpolazione coarse per grandi dabs; spunto per il futuro MicroDetail, ma no lateral displacement e nessun layer/paint sul microlevel; non considerare già risolto il nostro caso. https://sculptforge.app/guide/micro-levels.html
6. **Compressione lossless:** valutare Zstd fast/LZ4 su Undo e payload freddi dopo profiling di veri blocchi float3 e float4; MB/s su corpus esterni non sono un benchmark per sculpt. https://github.com/meta/zstd
7. **Blender 5.4 PBVH:** Reduce Sculpt Buffers merged, meshlets ancora WIP al 12 novembre; separare output GPU da solve/commit CPU; target preciso 5.4 SHA prima di portare. https://projects.blender.org/blender/blender/issues/160654 e https://projects.blender.org/blender/blender/pulls/166181

## 10. Piano Codex e criteria go/no-go
**G0 — Baseline:** stampare SHA, toolchain, driver/flags e creare fixture 100k/1M/10M/60M campioni; stress >100M solo quando fattibile. Log per phase: prepare, commit, CCG rebuild, Memfile decode, solve, publish, normals, stitch, bounds, Undo, RSS/VRAM e fallbacks LOCAL/METADATA/GLOBAL. A/B binario stabile e candidate, p50/p95 e cold/warm.
**G1 — Reliability:** riprodurre Undo Polypaint e fault-injection commit, poi implementare transazioni isolate senza modificare stable.
**G2 — Interactive:** provare grid->node index + Strength A, poi B/C soltanto se necessario; un Undo per gesto.
**G3 — Detail:** estrarre l'helper preesistente senza cambiare i risultati, estendere a un solo operatore Pose/Armature.
**G4 — 5.4:** compilare base 5.4 stock a SHA fisso e confrontare il suo PBVH/Undo con il nostro; porting solo dei percorsi che superano A/B.

Test obbligatori: save/reopen, EDIT/SCULPT switch, Undo cross-level, hidden/locked/solo layers, Strength -1/0/1, mask/FaceSets HR, Bake Visible preserving hidden, no spikes/cracks/NaN; performance e memory non peggiori in modo riproducibile negli scenari frequenti. Ogni PR: WHY, FILES, SHA, FLAGS, TEST REALI, BENCHMARK, KEEP/REVISE/ROLLBACK.

## 11. Collegamenti
[Master Roadmap](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/feature/pre54-symmetry-remesh-fixes-v01/doc/sculpt_extended/MASTER_ROADMAP_CODEX_20261108.md) — [Performance 5.4](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha-release-baseline/doc/sculpt_extended/MIGRATION_54_PERFORMANCE_PRESERVATION_20261106.md) — [#6 Poseable](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/6) — [#14 Remesh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/14) — [#15 Symmetrize](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/15).
