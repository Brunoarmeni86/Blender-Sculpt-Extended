# Sculpt Extended 5.4 — PERFORMANCE PRESERVATION CONTRACT
**Data:** 2026-11-06  
**Decisione utente:** nella migrazione 5.4 vanno preservati **tutti i miglioramenti prestazionali validati dello Sculpt**, non solo le feature. Migliorie upstream possono sostituire percorsi custom soltanto dopo prove A/B su scene equivalenti.  
**Branch baseline sorgente:** `migration/blender-5.4-alpha-release-baseline` = derivato dal tag **`v5.2.3-sculpt-extended-20261104`**, SHA `09a16d4b8482200101cb30728881ca4524edf019`. Questo ramo contiene per ora la **5.2.3**, NON il porting 5.4.  
**Branch di pianificazione precedente:** `migration/blender-5.4-alpha`, derivato da `main`. **NON deve essere assunto come baseline funzionale**, perché `main` e il tag release divergono.
**Confronto GitHub al 2026-11-06:** `main...v5.2.3-sculpt-extended-20261104`: **207 commit ahead / 40 behind** (due linee divergenti). Prima del porting, fare audit delle modifiche rilevanti uniche in entrambe le linee. Non sostituire la base release con `main` e non perdere fix della release 4 novembre.

## Regola di migrazione
Una feature può dirsi *portata* solo se:
1) comportamento/scultura e dati sono corretti;
2) velocità e uso memoria **non regrediscono in modo significativo** rispetto alla release Windows 4 novembre **nelle stesse scene e condizioni**;
3) fallback e feature flag continuano a funzionare dove necessari;
4) eventuale cambio di algoritmo ha prova numerica/visiva, Undo/Redo e benchmark cold/warm.

**Nessun cherry-pick automatico dei file vecchi:** le nuove implementazioni Blender 5.4 possono essere migliori, ma occorre **preservare il risultato misurato**, non necessariamente ogni vecchio algoritmo. In caso di regressione tenere il ramo rapido custom / fallback, isolato per A/B, oppure rinviare il porting.

## Inventario minimo da proteggere (stato da ricontrollare esattamente sul tag)
Legenda: **Presente** = percorso sorgente/documentato in release; **integrato** = dichiarato nel tracker 2026-11-02, ma richiede verifica sui binari 2026-11-04; **opt-in** = non assumere attivo di default; **proposto** = non confondere con implementazione.

| Area prestazionale | Implementazione / comportamento da non perdere | Ancore sorgente / test | Rischi 5.4 |
| --- | --- | --- | --- |
| **Commit locale Sculpt Layers** | Aggiorna solo griglie sporche quando `route=LOCAL`; dirty-grid journal, solve/publish locali, CCG/PBVH preservati nei casi supportati | `blenkernel/intern/multires.cc`; `LAYER_COMMIT_ROUTE`, `COMMIT_KEEP_CCG_V1`, `LAYER_COMMIT_PARALLEL_PUBLISH_V1` | Invalidazione depsgraph e PBVH, cambi API Multires/CCG |
| **Solve/publish efficiente** | Cache frame/tangenti, fast path griglie coerenti, allocazione e publish più localizzati, batching cache invalidations | `LAYER_COMMIT_FRAME_CACHE_V4_GRID_BATCH_BUILD`, `LAYER_COMMIT_FAST_TANGENT_SOLVE_V1`, `LAYER_COMMIT_CACHED_GRID_HOT_LOOP_V1` | Nuove strutture CCG, semantica del livello corrente, dirty bits |
| **Sparse layers** | Griglia senza payload significa displacement **zero implicito**, allocazione lazy, cache support index, riuso delta; conversione legacy delle griglie zero al caricamento dove abilitata | `multires_sculpt_layers.cc`, `LAYER_SUPPORT_INDEX`, `LAYER_WEIGHT_HOT_DELTA`; tracker performance | Regressione RAM, iterazioni O(mesh totale), materializzazione eager |
| **Memoria e cache budget** | Cache dimensionate in funzione di RAM disponibile, limitazione working set, isteresi, invalidazione e fallback | `multires.cc`, tracker 2026-11-02, flags RAM/cache | Cache thrash, page fault e spreco RAM su progetti densi |
| **Smooth Multires a livello inferiore** | Riuso del riferimento HR dopo il primo stroke, senza rifare costosi rebuild globali per ogni pennellata; fallback nativo per stack incompatibili | `sculpt_layer_delta_smooth_v01.hh`; `FEATURE_DEEP_DIVE_20261030.md` | Regressione alla prima pennellata o dopo cambio livello |
| **Adaptive Stroke / accumulo** | Distribuzione timbri e accumulo controllato del percorso proprietario; integrare curve pressione 5.3 preservando il risultato | `editors/sculpt_paint/paint_stroke.cc`, `SCULPT_ADAPTIVE_STROKE_V1/V2`, `sculpt.cc` | Densità dab, pressione/spaziatura, CPU per stroke |
| **Undo specializzato** | Fast path per swap/restore locale, same/cross-level, first-stroke, evitare copie geometriche e memfile ridondanti quando possibile | `editors/sculpt_paint/mesh/sculpt_undo.cc`, `SCULPT_LAYER_UNDO_FAST_STANDARD_V2`, `LAYER_UNDO_ANCHOR` | Nuovo core Undo 5.3/5.4, snapshot Polypaint, CCG rebuild |
| **PBVH / redraw / normali locali** | Dirty nodes, ripristino e aggiornamento solo aree effettive, cache e layout VBO personalizzati; evitare fallback globali a ogni dab | `draw/intern/draw_pbvh.cc`, `SCULPT_MESH_V3_LOCAL_NORMALS` (commenti), `BLENDER_MULTIRES_GEOMETRY_BUFFER`, `CompactMeshNodeData` | **Reduce Sculpt Buffers** in 5.4 cambia profondamente la cache |
| **Polypaint HR** | Master persistente a risoluzione massima, sparse/lookup; evitare rebuild, conversioni e memfile full-res non necessari | `multires_sculpt_layers.cc`, `sculpt_undo.cc`, `draw_pbvh.cc` | Il collo di bottiglia memfile Undo non è già risolto universalmente |
| **Pose/Move/Rotate/Scale** | Commit e redraw locali dove possibile, pivot metadata-only, falloff regionale, trasporto dettagli senza spikes | `sculpt.cc`, file Pose e Transform nativi/custom; tracker performance | Perdita beneficio regionale o ritorno di spikes/cerniere |
| **Shape key, paint preparation** | Shortcut e cache valide solo quando topology/session consentono il fast path | `multires_sculpt_layers.cc`, `SHAPEKEY_SUPPORT_CACHE_V2_1B`, `SHAPEKEY_BROAD_FAST_V271` | API shape keys/cache, invalidazioni incomplete |

### Ottimizzazioni da NON promuovere automaticamente a default
- `BLENDER_SCULPT_WEIGHT_HOT_SKIP_STITCH` / `BLENDER_SCULPT_LAYER_REGION_V01`: **opt-in** in tracker; test severi delle seam.
- `MULTIRES_GEOMETRY_BUFFER_V01` e `CompactMeshNodeData`: esperimenti da confrontare con il nuovo percorso 5.4, non da trapiantare ciecamente.
- Cache progressiva del primo commit: nel tracker una prova ha **peggiorato** il tempo (342,6 contro 161,8 ms); percorso disabilitato nella configurazione studiata.
- Compattazione legacy **durante** stroke: regressione misurata; lasciare inattiva salvo esplicita prova contraria.
- Migrazione legacy regionale sul percorso caldo: nel tracker scansioni fredde 40–390 ms contro ~1,2 ms del Preserve caldo; disabilitata nella configurazione studiata.
- P1/P3/P4/P5/P6/P7/P8/P10 nel tracker includono anche proposte, workaround o parziali, **non** considerarli tutti già implementati.

## Evidenze del tracker — SOLO specifici test storici, non promesse
Fonte: `doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md` nel tag release.
- Su una scena, un **Commit LOCAL** dopo G/R/S misurato a **395,439 ms**; attese molto più elevate erano dovute a Undo memfile Polypaint / rebuild CCG.
- Sul benchmark `zombie dragon.blend`, selezione layer mediana **0,0018 ms**, Strength denso mediana **3291 ms** (candidato, senza guadagno dimostrato rispetto a 3228 ms stabile).
- Percorso con cache calda: un commit misurato **161,8 ms cold / 107,4 ms hot**.
- Commit su 183.415/424.456 grid, 52,77M campioni: solve **914,978 ms**, publish **1057,888 ms**, totale **2072,844 ms** in quel checkpoint.
- Test memoria: working set circa **15 GB vs 23 GB** e private circa **29 GB vs 39 GB** in test precedenti, **non** un confronto universale.
- `FEATURE_DEEP_DIVE_20261030.md`: in uno Smooth a livello basso il precedente commit prima del secondo stroke era circa **58,8 s**, path ottimizzato **30,8 ms** in un confronto focalizzato, non una statistica generale.
- Persistono casi critici: memfile Undo Polypaint fino a **58,3 s** e global rebuild CCG; NON dichiararli già risolti.

Tutti i numeri devono essere **rifatti sulla release 4 novembre**, con log, scene, versione Windows/driver, flag, cold/warm e quantili, prima di usarli come baseline di accettazione.

## Bug prioritari da preservare/correggere in migrazione — 8 novembre 2026

- **P0 [#12](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/12):** passando da Combine Native + Reference alla modalità non combinata si può verificare aumento di Strength riportato dall'utente. Da audit statico la modalità non combinata **non equivale automaticamente alla sola simmetria nativa**: se `Use Reference` resta attivo, gli assi locali vengono ancora interpretati nel frame custom. Audit matrice pass, unicità impronte e Feather ON/OFF; test su layer e Multires senza regressioni di stroke.
- **P1 [#13](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/13):** G/R/S e gizmo devono essere allineati e disponibili nel giusto contesto senza compromettere shortcut normali; snap interattivo, offset dell'anchor e rebind esplicito da provare senza costi di ricostruzione della mesh. Due operator Python parzialmente sovrapposti e timer sul cursore richiedono unificazione/rivalutazione.
- Check di progetto: la **migrazione non deve trascinare questi problemi come comportamento accettato**; è ammissibile una patch sul fork stabile una volta riprodotta e collaudata, ma il confronto di prestazioni deve restare ancorato alla release 4 novembre.

### Priorità P1 — Remesh con mask field continuo e performance (issue #14)

[Issue #14](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/14) registra il requisito dell'8 novembre: m=1 ⇒ **nessuna mutazione** della parte interamente mascherata (geometria/topologia/attributi); m=0 ⇒ remesh a risoluzione piena impostata; 0<m<1 ⇒ transizione adattiva di densità, conservazione del bordo e movimento proporzionale all'influenza `1-m`. La V01 protegge una faccia se un singolo vertice ha mask>1e-6; nessuna transizione di densità e target uniforme. Non confondere il remesh locale BMesh con il voxel remesh volumetrico. Audit di scalabilità: BVH interamente globali e fino a 24 iterazioni sulla mesh; il codice V02 deve introdurre gestione di regioni sporche, controlli non-manifold/intersezioni **prima della pubblicazione** e A/B verificati. La protezione dei dati della maschera ha priorità sulla velocità. Nessun porting/patch ancora eseguito.

## Benchmark: blocchi obbligatori e priorità
Registrare per ogni campione: SHA sorgente, build, CPU/GPU/driver, scena, livello Multires, modificatori, brush, layer, maschere, flags, RAM/VRAM, `PASS/FAIL`.

**B1 — Stroke Sculpt:** Draw, Clay, Smooth, Shift+Smooth, Pose, Elastic/Grab; start/cold stroke, dabs/s o ms/dab, 10 stroke continui, p50/p95, frame time viewport. Sulle mesh dense catturare pause > 100ms e saturazione RAM/VRAM.

**B2 — Layer workflow:** selezione A/B 20 volte, Strength 0→1→0 su layer denso e sparso, Commit successivi, Bake Visible→Base, merge, duplicate, Undo dopo ciascuno; separare tempo UI, solve, publish, CCG e PBVH.

**B3 — Multires & detail:** lavoro L0/Ln, cambio livello e ritorno, primo e secondo Smooth, Pose sotto livello massimo→massimo, spostamenti G/R/S su porzioni mascherate, identità corner; confronto geometria e preservazione micro-dettaglio.

**B4 — Paint/Undo:** Polypaint HR su L0/master, 4K/UDIM, stroke e Undo/Redo, memfile size + decompression time, numero rebuild CCG, costo di colore in viewport.

**B5 — Memoria & display:** scene ~100k, 1M, 10M, 60M campioni, stress 100M+ solo se fattibile; working/private/RSS, RAM free, VRAM, GPU redraw, cold draw, update Normals per regione, page faults, crash. Non confondere stroke fluido con cambio livello veloce.

**A/B/C:** A release 5.2.3 Windows 4 novembre; B nuova build 5.4 custom; C 5.4 Alpha ufficiale. Stesse scene e flag equivalenti; confrontare anche B con feature flags OFF/ON. Almeno 5 ripetizioni per scenario e separare cold/warm.

### Gate quantitativo proposto (da approvare dopo baseline)
- Prima di promuovere un checkpoint, nessuna regressione **stabile e riproducibile** di p50/p95 stroke, commit, Undo, UI o memoria nei casi di uso frequente.
- Differenze ≥10% su tempi mediani oppure ≥15% su p95 diventano **blocker per analisi** (soglie provvisorie, NON limiti già approvati); peggioramenti minori ma ripetuti su file grandi vanno comunque discussi.
- Funzioni e velocità recuperate per ogni milestone **M1–M8**, non soltanto a fine migrazione. Un fallimento costante ⇒ fallback/rollback sul singolo percorso.
- L'equivalenza di dati, il salvataggio e le assenze di spikes non sono negoziabili.

## Procedura di controllo conflitti e provenienza
1. **Tag release 4 novembre** come sorgente autoritativo delle performance da preservare.
2. **`main` divergente**: audit commit, documenti e fix esclusivi, integrare solo modifiche davvero pertinenti dopo confronto (non perdere ulteriori migliorie utili).
3. **5.4 upstream SHA fissata**: compilare prima stock 5.4, poi spostare moduli custom per layer/Undo/PBVH in modo incrementale.
4. Ogni ottimizzazione deve avere una riga `old code -> upstream replacement/custom port -> benchmark -> keep/replace/rollback`.
5. Annotare separatamente: **presente nel codice**, **abilitata nella build**, **confrontata A/B**, **regressione**, **da verificare**. Non attribuire ai pacchetti rilasciati esperimenti opt-in o cambi successivi senza prova.

## Riferimenti
- [Tracker prestazioni del 2 novembre](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/v5.2.3-sculpt-extended-20261104/doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md)
- [Sorgente release 4 novembre](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/v5.2.3-sculpt-extended-20261104)
- [Feature deep dive / benchmark Smooth](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/v5.2.3-sculpt-extended-20261104/doc/sculpt_extended/FEATURE_DEEP_DIVE_20261030.md)
- [Memo PBVH 5.4](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/main/doc/sculpt_extended/PBVH_DRAW_54_MEMO_20261106.md)
- [Piano di migrazione 5.4](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha/doc/sculpt_extended/MIGRATION_BLENDER_54_ALPHA_20261106.md)

**Stato attuale:** solo baseline + documentazione; nessun sorgente 5.4 migrato/compilato.
