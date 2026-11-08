# Sculpt Extended — Master Roadmap e handoff per Codex
**Data:** 8 novembre 2026 · **Stato:** documento di coordinamento, NON una build o una certificazione di fix.  
**Repository:** [Brunoarmeni86/Blender-Sculpt-Extended](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended)  
**Branch di laboratorio:** `feature/pre54-symmetry-remesh-fixes-v01`  
**Baseline da preservare:** tag `v5.2.3-sculpt-extended-20261104`, build Windows distribuita il 4 novembre.

## 1. Decisione e regole operative
L'utente chiede **unificare tutta la progettazione** per permettere a Codex di correggere e riscrivere codice successivamente, **senza obbligo di risolvere tutto ora**.

- **Non modificare** `main`, il tag release e gli artefatti stabili. Nessun merge/nuova release senza verifica e approvazione.
- Conservare **tutte le ottimizzazioni validate** della release, non necessariamente lo stesso algoritmo se la nuova versione è dimostrabilmente migliore.
- La prima iterazione riguarda la base 5.2.3; le correzioni convalidate verranno selettivamente portate verso Blender 5.4. La ricerca e il confronto upstream 5.4 possono procedere in parallelo, ma il porting di codice non è ancora eseguito.
- **Tre branch diversi:** `feature/pre54-symmetry-remesh-fixes-v01` è il laboratorio WIP; `migration/blender-5.4-alpha-release-baseline` deriva dal tag 5.2.3 ed è il riferimento per preservare le prestazioni; `migration/blender-5.4-alpha` è un piano nato da `main`, NON una build 5.4.
- **Attenzione alla divergenza:** secondo il confronto GitHub già effettuato, la release e `main` hanno 207 commit solo nella release e 40 solo in `main`. Eseguire un audit prima di qualsiasi merge: il tag distribuito è la base per feature e prestazioni.
- Il presente documento è l'**indice canonico**. I documenti precedenti restano fonti tecniche o audit storici da confrontare con il tag attuale.

### Livelli di evidenza
**REPORT UTENTE** = comportamento segnalato ma non riprodotto automaticamente; **SORGENTE CONFERMATO** = condizione direttamente visibile nel codice, non necessariamente bug dell'eseguibile; **STORICAMENTE MISURATO** = prove di commit precedenti non replicate oggi; **RISCHIO** = ipotesi; **PROPOSTA** = non implementata; **WIP** = patch non validata; **VALIDATO** = Blender reale + verifica dati + prestazioni documentate.  
Ogni PR/commit deve dichiarare stato, SHA, scene e parametri dei test; compilare non equivale a validare.

## 2. Correzioni e affidabilità: priorità reale
| ID | Priorità | Risultato richiesto | Stato attuale / fonti |
|---|---|---|---|
| **S01** | **P0** | Doppia simmetria: nessuna Strength additiva imprevista dopo Combine OFF; stato Native/Reference/Combined non ambiguo | [Issue #12](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/12). Report utente, mismatch degli assi nel sorgente, piccola patch Python **WIP**. Test per pass, Feather ON/OFF, Brush, Multires e Undo. |
| **S02** | **P0/P1** | Symmetrize dal gizmo: copia affidabile della topologia dal lato sorgente, anche quando deformato; raccordo senza errori | [Issue #15](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/15). Corrispondenza seam parziale WIP; copie di forma e connettività da distinguere. |
| **S03** | **P1** | Remesh protetto da maschera continua: m=1 non tocca coordinate/topologia/attributi; m=0 remesh pieno; transizione graduata 0<m<1 | [Issue #14](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/14). Patch BMesh sperimentale; validate manifold, transizioni, UV, interpolazioni e performance. Il vecchio operatore NON è un vero remesh volumetrico. |
| **S04** | **P1** | Gizmo sempre controllabile nel contesto appropriato: G/R/S selezionano l'handle e l'interfaccia corretti; snapping e anchor/rebind affidabili | [Issue #13](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/13). Due percorsi trasformazione, cursore 3D temporaneo, anchor non supportato su Multires>0; piccole modifiche WIP. |
| **D01** | **P0** | Undo e Polypaint HR senza checkpoint memfile o rebuild CCG ridondanti; nessuna perdita di dati | [Audit aggiuntivo](PRE54_ADDITIONAL_WEAK_POINTS_AUDIT_20261108.md). **STORICAMENTE MISURATO:** fino a 58,3 s Undo per caso di preparazione Polypaint, contro Commit LOCAL di circa 395 ms. |
| **D02** | **P0** | Protezione operazioni strutturali: Join, duplicazione, conversione, apply modifier/scale, cambio topologia, save/reopen | [Audit aggiuntivo](PRE54_ADDITIONAL_WEAK_POINTS_AUDIT_20261108.md). Matrice supportato/bloccato/da implementare; checksum per layer L0/HR, colori, Face Sets. Apply Scale ha già protezioni nella release: non riaprire il vecchio bug senza nuove prove. |
| **D03** | **P0 (infrastruttura)** | Suite regression e benchmark automaticamente ripetibili, file fixture, gate qualità | [Fixture WIP](tests/pre54_symmetry_remesh_regression.py). I test aggiunti NON sono ancora stati eseguiti su Blender, non certificano le patch. |
| **P01** | **P1** | Strength e selezione layer più fluide: commit solo per modifica pendente, singola transazione durante il trascinamento, Undo corretto | [Tracker 2 novembre](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/v5.2.3-sculpt-extended-20261104/doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md). Percorsi metadata/delta esistono; evitare lavoro superfluo. |
| **P02** | **P1** | Una semantica per selezione RNA e operatore, lista layer scalabile, gruppi e visibilità coerenti | Come P01, con test 10/100/500 layer e click/Undo/cambio livello. |
| **P03** | **P1** | Cache/dirty/PBVH: tracciare ogni fallback GLOBAL e ragioni, fingerprint completo degli input Subdiv, nessuna cache stale | [Audit aggiuntivo](PRE54_ADDITIONAL_WEAK_POINTS_AUDIT_20261108.md). L'hash non include esplicitamente crease/boundary/quality; **RISCHIO**, verificare invalidazioni esterne prima di correggere. |
| **P04** | **P1** | Pose, S/R/T, Elastic/Grab: falloff geodesico, trasformazione distribuita, conservazione dettaglio HR e anti-spikes | [Tracker release](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/v5.2.3-sculpt-extended-20261104/doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md). Esistono già falloff regionali, weighted transform e fix di trasporto detail; estendere, non riscrivere indiscriminatamente. |
| **P05** | **P1** | Ottimizzazioni PBVH/CCG e RAM, compatibilità con Reduce Sculpt Buffers 5.4 | [Memo upstream PBVH](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/main/doc/sculpt_extended/PBVH_DRAW_54_MEMO_20261106.md). Copiare la nuova cache 5.4 in `draw_pbvh.cc` senza adattamenti romperebbe codice custom. |
| **P06** | **P2** | Registro unico flag sperimentali, fallback, combinazioni compatibili, deprecazioni, log diagnostici | Proposta: registry feature→default→dependencies→fallback→validation. |

### Architettura condivisa proposta per #14 e #15
Creare un modulo concettuale **Mask / Geometry Constraints**, riutilizzato da due algoritmi differenti:
1. **Surface Remesh:** split/collapse/flip con dimensione edge locale e banda di raccordo; hard lock sulle facce totalmente protette e sui confini condivisi.
2. **Symmetrize Copy Shape:** trasporto coordinate senza riscrivere topologia, sfruttando mappe di corrispondenza e residui.
3. **Symmetrize Copy Topology:** copia connectivity/face-corner attributes dalla metà sorgente e raccorda solo la regione libera; supportare gradualmente più contorni, isole, fori.
4. **Voxel Remesh volumetrico locale:** progetto separato e più oneroso; non attribuire capacità di fondere volumi alle patch BMesh attuali.
5. Usare staged mesh + validate before publish per evitare mesh nonmanifold, T-junction, self-intersections, drift delle aree protette o dati persi. Proteggere la maschera completa **esattamente**, non solo visivamente.

## 3. Ottimizzazioni già sviluppate da preservare nella migrazione
| Sistema | Fast path/proprietà da conservare e verificare |
|---|---|
| **Multires Sculpt Layers** | Layer sparse con zero implicito, Commit LOCAL per griglie dirty, snapshot e metadata-only selection, Bake Visible→Base che conserva layer hidden. |
| **Stroke** | Adaptive Stroke personalizzato; distribuzione dei dab e accumulo, insieme alle future curve di pressione 5.3 senza raddoppi di Strength. |
| **Smooth ai livelli inferiori** | Cache/riferimento HR riutilizzato, preservazione dei dettagli al ritorno al massimo livello. |
| **Undo** | Restore locale/same-cross-level, compressione e gestione transazioni, first stroke; attenzione ai costi residui memfile Polypaint. |
| **Polypaint/Face Sets HR** | Master persistente al livello più alto, visualizzazione ai livelli bassi, payload sparse, dati e Undo coerenti. |
| **PBVH** | Dirty nodes, normali/bounds locali, cache e draw custom con budget RAM dinamico e fallback garantiti. |
| **Pose e Transform** | Falloff regionale, pesi cached per gesture, trasporto detail low→high senza spikes. |
| **Memoria** | Compattazione dei payload legacy solo in contesti sicuri, cache adattive e isteresi. |
| **Performance gate** | Confronto A/B release Windows 4 novembre vs nuova build; baseline p50/p95, primo stroke, commit/undo/level change, RAM/VRAM. |

**Non promuovere a default i precedenti esperimenti regressivi** (cache progressiva primo commit, compattazione durante stroke, migrazione legacy durante stroke, skip stitching non validato); leggere il [contratto performance](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha-release-baseline/doc/sculpt_extended/MIGRATION_54_PERFORMANCE_PRESERVATION_20261106.md).

**Nota sugli audit del 16 ottobre:** i bug storici B01/B02 Flatten/Merge, B03 contabilizzazione Face Set Undo, B04 Bake sparse e B05 Apply Scale mostrano nuove protezioni nella release 4 novembre. Sono vecchi risultati su checkpoint precedenti; non dichiararli aperti senza riproduzione. Anche B06 e B07 sono descritti nel lavoro successivo: verificarne lo stato effettivo del tag invece di riapplicare patch.

## 4. Funzionalità e sviluppo futuro già in backlog (NON bug)
| Ambito | Stato attuale e sviluppo residuo | Riferimento |
|---|---|---|
| **Poseable Symmetry e Quick Pose** | Esistono frame di riferimento e parte della simmetria topologica/Multires. Manca simmetria degli stroke sulla forma posata usando identità/rest pose e CCG, senza duplicare effetti. | [#6](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/6) |
| **MicroDetail indipendente dalla risoluzione di lavoro** | Sparse layer esistenti ≠ microgrid compresse e residenti a LOD variabile. Ideare microtile/ownership/Undo/bake e preservare dettaglio HR. | [#7](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/7) |
| **Adaptive Sculpt LOD e Surface Projection** | Livelli Multires globali e aggiornamenti locali presenti; LOD CCG/PBVH regionale non implementato. Testare mixed-level seam, brush refinement, mask/Face Set lock e Undo. | [#8](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/8) · [Specifica](ADAPTIVE_SCULPT_LOD_PROJECT_20261101.md) |
| **NanoMesh nei layer** | Progetto per istanze stabili su mask/Face Set, Brush Paint/Erase/Density, legame superficie, output istanze/VDM/ibrido. Attualmente è una specifica, non una funzione attiva. | [#9](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/9) |
| **Funzioni portabili da ZBrush/SculptForge** | PolyGroups-like su Face Sets HR; Rake/Roll/Ribbon, Morph, Selection Sets, HPolish/DamStandard ecc.; riusare brush assets e funzioni native quando possibile. | [#10](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/10) |
| **Surface Draw e Texture Paint** | Integrare criticamente codice sperimentale ufficiale 5.4 in Texture Paint, preservando Polypaint HR/UDIM e separando le modalità. | [#11](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/11) |
| **Pennelli ufficiali 5.3** | Curve pressione Hardness/Auto Smooth/Spacing, UI/asset per brush e interazione con Adaptive Stroke. Pericoloso importare in blocco vecchi file DNA; testare salvataggio e Undo. | [Memo migrazione 5.4](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha/doc/sculpt_extended/MIGRATION_BLENDER_54_ALPHA_20261106.md) |
| **PBVH 5.4** | Reduce Sculpt Buffers già introdotto upstream ma meshlets WIP al momento dell'audit; adottare come esperimento dopo benchmark e audit delle cache custom. | [PBVH memo](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/main/doc/sculpt_extended/PBVH_DRAW_54_MEMO_20261106.md) |

## 5. Sequenza operativa raccomandata a Codex
**Fase 0 — Baseline e prove.** Fissare SHA tag, pacchetto Windows e configurazione flags. Confrontare sorgenti del branch WIP con release. Verificare sintassi Python, eseguire Blender headless e fixture interattive reali su copie .blend. Registrare prima i bug riproducibili e i costi p95.

**Fase 1 — Dati e regressioni gravi.** S01 (#12), D01 Undo/Polypaint, D02 data integrity, matrice compatibility e guardie transazionali. Bloccare eventuali percorsi realmente distruttivi.

**Fase 2 — Riorganizzazione strumenti.** Condividere i vincoli della maschera; migliorare S02/S03, corrispondenza topologica, collar, snap/gizmo S04 e UI. Per ogni patch verificare source/target, estremi m=0/m=1, gradienti, attributi.

**Fase 3 — Prestazioni e qualità Sculpt.** P01/P02 layer UI, P03 fingerprint/dirty, P04 Pose/Transform, P05 PBVH/undo/mem. Confrontare A/B e scegliere percorsi custom/upstream per merito verificato.

**Fase 4 — Migrazione 5.4.** Una volta disponibili patch convalidate, portarli selettivamente da `feature/pre54...` alla nuova base upstream 5.4 fissata a SHA; testare subito DNA/RNA/Blender file/Multires/Undo e le prestazioni **a ogni checkpoint**, non soltanto a fine porting.

**Fase 5 — Roadmap evolutiva.** #6–#11 e pennelli 5.3, in piccoli rami indipendenti; mantenere compatibilità con tutti i sistemi esistenti. Non usare queste aggiunte per ritardare i fix critici.

Non è necessario completare sequenzialmente tutte le fasi prima di esplorare upstream: questa sequenza è un criterio di rischio e accettazione, non una proibizione di sviluppo parallelo.

## 6. Matrice minima test prima di dire «corretto»
| Ambito | Test obbligatori |
|---|---|
| **Simmetria/Strength** | Local, Reference, Combined; Combine ON→OFF; Feather ON/OFF; piano spostato/ruotato; passi sovrapposti; primo/secondo stroke; Undo/Redo. |
| **Gizmo** | G/R/S con tool attivo/inattivo; UI corrispondente; Snap Ctrl/Shift/conferma/Esc; anchor/rebind, Multires, viewport ortho/perspective. |
| **Mask Remesh/Symmetrize** | m=0/0.25/0.5/0.75/1, fascia stretta/larga, facce totalmente protette immutate, sorgente intatta, UV/materiali/FaceSets/colori conservati; nonmanifold e intersezioni bloccati; più contorni. |
| **Sculpt Layers / HR** | 1/10/100 layer, sparse+densi, gruppi/hidden, L0↔Ln, Strength/Visible, Bake→Base, copia, Flatten/Merge, Edit Mode remap, Mask/FaceSet HR, Undo/Redo, save/reopen. |
| **Pose/Detail** | L0 e livello massimo, mani/articolazioni sottili, griglie di confine, Mask e FaceSets, simmetria e gruppi, ritorno al massimo senza spikes. |
| **Perf** | 100k/1M/10M/60M campioni con stress >100M solo dove fattibile; cold/hot stroke, p50/p95, level change, commit, Undo memfile, redraw PBVH, RAM/VRAM; build comparabili. |
| **Provenienza** | SHA, Blender version, OS, GPU/driver, fixture .blend, brush/flags, screenshot/log, PASS/FAIL, eventuale fallback attivo. |

**Non utilizzare i due script WIP come garanzia di sicurezza;** devono essere prima eseguiti e, se necessario, corretti. Una prova che fallisce ha priorità rispetto al design speculativo.

## 7. Commit, report e scelta
Per ogni task Codex annotare: **issue**, file/funzioni, criterio di accettazione, stato evidenza, SHA candidato, test eseguiti realmente, checksum/diff geometria, benchmark e decisione **keep/revise/rollback**. Se una patch degrada la scultura o rompe dati: rollback selettivo e documentazione; non eliminare ottimizzazioni funzionanti per uniformità col codice nuovo.

### Entrypoint per Codex
Leggere [`AGENTS.md`](../../AGENTS.md) e questo documento, poi [handoff #12–#15](PRE54_SYMMETRY_REMESH_FIXES_V01_20261108.md), [audit aggiuntivo](PRE54_ADDITIONAL_WEAK_POINTS_AUDIT_20261108.md), [performance 5.4](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha-release-baseline/doc/sculpt_extended/MIGRATION_54_PERFORMANCE_PRESERVATION_20261106.md) e le issue collegate.

**Stato alla pubblicazione:** documento/indice e codice WIP conservati su GitHub. **Non è stata eseguita alcuna compilazione, test Blender o benchmark con questo consolidamento.**