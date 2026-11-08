# Pre-5.4 — ulteriori punti deboli e miglioramenti da valutare con Codex
**Data:** 2026-11-08  
**Sorgente audit:** tag `v5.2.3-sculpt-extended-20261104` (sorgenti letti su GitHub).  
**Branch laboratorio:** `feature/pre54-symmetry-remesh-fixes-v01`.  
**Stato:** **solo analisi/proposte**, nessuna di queste nuove correzioni è stata implementata o collaudata. Non toccare `main` o il tag stabile; non far partire una migrazione a 5.4 prima che le modifiche abbiano benchmark/QA.

## Legenda
- **Confermato nel codice:** proprietà del sorgente/revisione, non automaticamente bug dell'eseguibile.
- **Misurato storicamente:** dato nel tracker del 2-4 novembre, non ripetuto ora sul binario finale.
- **Possibile rischio:** catena da riprodurre; non affermare che causi perdita dati.
- **Proposta:** funzionalità o ottimizzazione non realizzata.

## Priorità aggiuntive, oltre alle issue #12–#15
| ID | Priorità | Ambito | Stato / evidenza | Test e direzione |
|---|---|---|---|---|
| A01 | **P0** | **Undo/Polypaint HR, preparazione lazy e rebuild CCG** | **Misurato storicamente**: tracker 2026-11-02: in un workflow Undo 58,3 s con decodifiche memfile 25,9+32,3 s; rebuild CCG ~9,8–13,3 s. Commit locale dello stesso test ~395 ms. Code path complessi in `sculpt_undo.cc` e Polypaint. | Ridurre memfile multipli nella preparazione risoluzione/attributi, unificare checkpoint logici, preferire snapshot regionali/compressi con fallback corretto. Test first-stroke, Undo/Redo, Edit/Sculpt, save/reopen, layer nascosti, UV. Profilare tempi separati da commit/pennelli. |
| A02 | **P0** | **Integrità dati nelle operazioni strutturali** | **Rischio:** Join, apply modifier, make single-user, copia/duplicazione oggetti, cambio topologia, conversione, cambi asset potrebbero non ricollocare correttamente tutti i custom master del modifier; i vecchi report K03 non sono una prova sul tag finale. | Matrice di compatibilità per ogni operazione: `supportato/testato`, `bloccato esplicitamente`, `non implementato`. Round-trip .blend, digest di layer/Polypaint/Face Sets/hidden/L0, undo. **Non effettuare modifiche distruttive in-place senza staging/rollback.** |
| A03 | **P1** | **Strength e selezione Sculpt Layers** | **Confermato nel tracker:** commit preventivo anche senza coords dirty; `RNA` e operatore non unificati, UI Python per-riga. Su `zombie dragon.blend` Strength layer denso mediana 3.291 ms (3,291 **secondi**) in una configurazione; differenza vs 3.228 ms stabile nel rumore. Selezione metadati già veloce. | Unificare `has_pending_sculpt_transaction()`, evitare commit sui tick successivi allo stesso drag, UI interattiva begin/update/end con preview ed Esc, UIList/virtualizzazione. Verificare correttezza per maschere e layer diverso dopo Undo. |
| A04 | **P1** | **Fingerprint/cache Multires** | **Rischio statico:** `multires_sculpt_commit_base_fingerprint()` in `multires.cc` ~1288 include livello, coordinate, edge/corner/face connectivity; **non include esplicitamente crease, opzioni boundary/quality/evaluator**. Potrebbero esistere callback di invalidazione esterni, da auditare prima di segnalarlo come bug. | Ccache warm: cambiare crease/boundary/quality e impostazioni Subdiv senza muovere cage, confrontare hash, fast path e full recompute, checksum in posizione finale, segnalare causa di cache hit/miss. Verificare invalidazioni prima di aumentare fingerprint. |
| A05 | **P1** | **Pose, Scale/Rotate/Translate e microdettaglio** | **Parzialmente integrato / verifiche incomplete**: tracker 2026-11-03 integra weighted transform V01 con 1025 matrici/weight bin, Pose regional falloff e trasporto dettagli inferiori; test di equivalenza maschera, livelli, seam e deformazioni complesse ancora necessari. | Integrazione di pesi geodesici locali e trasformazioni con rotazione e scala pesate, cache per gesto; test arti lunghi, bend/knee, superficie sottile e crossing seam, rientro al livello più alto senza spikes. Misurare setup vs update (non ricalcolare blur per evento). |
| A06 | **P1** | **PBVH, memory budget, fallback GLOBAL vs LOCAL** | **Percorsi esistenti**: `LAYER_COMMIT_ROUTE`, journal griglie e cache adattive. **Misurato:** 52,77M campioni, commit ~2.073 ms (2,073 secondi) in un test; picchi page fault/publish e memfile residui. `Reduce Sculpt Buffers` 5.4 cambia architettura del draw. | Diagnostica per regione/feature, motivazione fallback; evitare cache globali e work non necessari. A/B sempre contro **release finale 4 novembre**; separare costi CPU, GPU redraw, RAM, VRAM, cache hit/miss, first stroke, p95. |
| A07 | **P1** | **Regressioni automatiche / compatibilità dei file** | **Proposta organizzativa**, non un bug: la release incorpora DNA/RNA e storage custom non presenti in Blender ufficiale, con numerose feature flag. La sola compilazione non garantisce che i dati sopravvivano. | Test CI Blender headless e test interattivi tracciati. Fixture `.blend` versionate, checksum dati base/layer/hidden/Polypaint/FaceSets, salvataggio+riapertura, cross-level Undo/Redo, crash/no-spikes, path mask/remesh/simmetria, confronto benchmark. |
| A08 | **P2** | **Coerenza feature flag, percorsi custom e documentazione** | **Confermato staticamente:** vari `BLENDER_*` flag, prototipi Python, percorsi C++ e fallback legacy possono essere abilitati a combinazioni difficili da riconoscere. Non è dimostrato un errore specifico di selezione. | Centralizzare registry `feature -> default -> dependencies -> compatible modes -> fallback -> diagnostics`. Output diagnostico riproducibile su startup e file. UI sperimentale separata; legacy chiaramente deprecato dopo QA. |
| A09 | **P2** | **Polypaint HR fast paths** | **Proposte precedenti, non nuove correzioni:** se livello paint == master, writer può evitare interpolazioni e aggiornare un campione diretto; quando alcuni pesi sono zero evitare scritture inutili, tenendo conto dell'arrotondamento IEEE (`-0.0` ecc.). | A/B numerico colori HDR/alpha, livelli, seam, UV/UDIM, Undo/Redo; *non* dedurre guadagno 9× da un kernel locale. |

## Correzioni storiche che NON vanno riaperte senza verifiche
Il vecchio audit del 16 ottobre (`doc/sculpt_extended/audits/2026-10-16/`) riguarda un checkpoint precedente, **non** certifica bug aperti nella release 4 novembre:
- **B01/B02** Flatten e Merge Down: nel tag corrente `flatten()` e `merge_down()` passano attraverso `merge_layers_validated()`; controllare test e non riutilizzare automaticamente il vecchio report come bug corrente.
- **B03** memoria Undo Face Sets: l'attuale `node_size_in_bytes()` include `grid_face_sets`.
- **B04** Bake Visible sparse: `bake_visible_to_base()` a Multires>0 instrada su `merge_visible_to_base()`, con preflight.
- **B05** Apply Scale: `object_transform.cc` contiene `sculpt_layer_scale_payloads()` con preflight e restrizione a scala uniforme positiva per Sculpt Layers.
- **H01** resample FaceSets: `faceset_resample_up()` valida `faceset_master_valid()` prima di procedere. Non dedurre da qui che tutte le corner case siano risolte senza test.
Queste sono **evidenze di modifiche del percorso**, non certificazioni di correttezza finale.

## Piano Codex suggerito
1. Prima eseguire test statici/test Blender sulla branch `feature/pre54-symmetry-remesh-fixes-v01`; non fidarsi delle patch WIP aggiunte qui.
2. Priorità di sviluppo: **A01 Undo/Polypaint**, **A02 integrity matrix**, **A07 test suite**, quindi **A03 Strength**, **A04 fingerprint/cache**, **A05 Pose**, **A06 PBVH**.
3. Per ogni candidato: creare fixture riproducibile, identificare SHA esatto, marcare `IDEA/PROTOTYPE/STATIC-VALIDATED/BLENDER-RUNTIME-VALIDATED/BENCHMARKED`, aggiungere fallback e misurare sulla release realmente distribuita.
4. Proporre PR piccole, non merge automatico; portare i fix validati su 5.4 soltanto dopo equivalenza dati e prestazioni.
5. Preservare l'architettura personalizzata non sostituendo in blocco `sculpt.cc`, `multires.cc`, `sculpt_undo.cc` e `draw_pbvh.cc`.

## Fonti interne
- `doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md` nel tag release.
- `doc/sculpt_extended/audits/2026-10-16/README.md` e `DEEP-REVIEW.md` (solo stato storico).
- `source/blender/blenkernel/intern/multires.cc`, funzioni fingerprint/cache.
- `source/blender/blenkernel/intern/multires_sculpt_layers.cc`, merge/flatten/bake/FaceSets.
- `source/blender/editors/object/object_transform.cc`, Apply Scale.
- `source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc`, storage Undo.
- [Handoff precedente #12–#15](PRE54_SYMMETRY_REMESH_FIXES_V01_20261108.md).
