# Sculpt Layers Performance Tracker

Data: 2026-11-02
Baseline sorgente: `1eaaa7d9fe8b7514edff6979dabebefa835e5202`
Ramo: `checkpoint/stable-20261027`

Questo documento e il registro operativo per le ottimizzazioni di Sculpt Layers. Distingue il
codice gia integrato dalle prove opt-in e dalle proposte ancora da misurare. Una proposta non
diventa "attuata" finche non supera confronto visivo, Undo/Redo, maschere, salvataggio e cambio
livello Multires.

## Stati

- **INTEGRATA**: attiva nella baseline e usata normalmente.
- **OPT-IN**: presente nel codice ma disabilitabile o ancora sperimentale.
- **PROPOSTA**: non implementata.
- **DA MISURARE**: il percorso esiste, ma manca un benchmark ripetibile sulla mesh reale.
- **SCARTATA**: provata e rifiutata; annotare sempre il motivo.

## Funzioni gia integrate

| Area | Stato | Comportamento presente | Traccia / codice |
|---|---|---|---|
| Selezione layer | INTEGRATA, DA MISURARE | Se la superficie CCG/PBVH resta valida, cambia solo l'indice attivo e richiede un redraw. Il rebuild geometrico e un fallback. | `LAYER_SELECT_OPERATOR_V03`, `LAYER_SELECT_METADATA_V01` |
| Strength locale | INTEGRATA, DA MISURARE | Dopo il commit applica alla superficie live solo la differenza di peso del layer. Il rebuild globale resta un fallback conservativo. | `LAYER_STRENGTH_LOCAL`, `rna_multires_visible_fast_refresh()` |
| Strength a zero | INTEGRATA | La transizione a zero rimuove il contributo precedente invece di essere trattata come aggiornamento vuoto. | `LAYER_STRENGTH_ZERO_FIX_V01` |
| Commit locale | INTEGRATA, DA MISURARE | Quando il commit locale riesce, conserva CCG/PBVH e limita il lavoro alle griglie sporche. | `LAYER_COMMIT_ROUTE route=LOCAL` |
| Undo layer | INTEGRATA, DA MISURARE | Frame nativi, percorso same-frame/cross-level e anchor iniziale evitano copie geometriche non necessarie. | `LAYER_UNDO_CROSS_LEVEL`, `LAYER_UNDO_ANCHOR` |
| Maschera | INTEGRATA | I commit coordinate-only preservano la maschera fine esistente. | `MultiresReshapeContext::preserve_paint_mask` |
| Cache delta Strength | OPT-IN | Cache runtime limitata per riusare la conversione del delta del layer. Ha invalidazione esplicita e budget RAM. | `LAYER_WEIGHT_HOT_DELTA`, `LAYER_WEIGHT_MULTI_V01` |
| Indice griglie occupate | INTEGRATA, DA MISURARE | Un indice runtime del layer attivo evita di scandire tutti i descrittori a ogni evento Strength. Attivo per default; `BLENDER_SCULPT_LAYER_SUPPORT_CACHE=0` esegue A/B. | `LAYER_SUPPORT_INDEX` |
| Regione sparsa Strength | OPT-IN | Costruisce l'anello di facce partendo dalle sole griglie con payload per layer radi. | `BLENDER_SCULPT_LAYER_REGION_V01`, `LAYER_REGION_V01` |
| Skip stitch su cache hit | OPT-IN | Su un hit esatto prova ad aggiornare le normali senza stitching delle posizioni. Richiede verifica severa delle cuciture. | `BLENDER_SCULPT_WEIGHT_HOT_SKIP_STITCH` |

## Costi residui individuati nel codice

### Selezione layer

1. L'operatore chiama ancora `commit()` prima di ogni cambio reale, anche quando il CCG non ha
   coordinate, maschere o visibilita sporche.
2. La selezione della sidebar non richiede piu `OPTYPE_UNDO`; resta da misurare il costo della
   transazione di commit quando esistono modifiche pendenti reali.
3. Il pannello Python costruisce manualmente una riga completa per ogni layer a ogni redraw. Con
   molti layer il costo UI puo superare quello del cambio indice.
4. Il percorso RNA e quello dell'operatore non condividono ancora una singola funzione di
   transazione; questo aumenta il rischio di divergenze e lavoro duplicato.

### Strength

1. Il setter esegue `commit()` a ogni evento del cursore. Dopo il primo evento, molti eventi
   successivi possono non avere alcuna modifica sculpt pendente.
2. La ricerca delle griglie con payload puo attraversare tutti i grid del layer a ogni evento.
3. La ricerca dei nodi PBVH interessati controlla i nodi e le relative griglie a ogni evento.
4. Stitching, normali e bounds sono aggiornati durante ogni variazione intermedia del cursore.
5. Il pannello usa una proprieta RNA standard: manca una transazione esplicita begin/update/end
   che possa separare anteprima interattiva, Undo e consolidamento finale.

## Proposte prioritarie

### P1 - Fast path senza commit per selezione pulita

**Stato: PROPOSTA**

Prima del commit, verificare in modo conservativo:

- SculptSession e CCG validi;
- nessuno stroke/filter attivo;
- `dirty.coords`, `dirty.masks` e `dirty.hidden` falsi;
- nessuna transazione layer pendente non rappresentata dai dirty flag.

Se tutte le condizioni sono vere, cambiare direttamente l'indice attivo, disattivare REC secondo
la policy corrente e inviare solo redraw/notifica metadata. Il percorso esistente resta fallback.

**Rischio principale:** perdere l'associazione di una pennellata pendente al vecchio layer.
**Prova necessaria:** selezione dopo Draw, Smooth, maschera, Undo, cambio livello e apertura file.

### P2 - Selezione non geometrica fuori dall'Undo pesante

**Stato: INTEGRATA, DA MISURARE**

L'operatore di selezione non usa piu `OPTYPE_UNDO`. Il commit delle eventuali coordinate pendenti
avviene prima del cambio indice; la pennellata conserva il proprio Sculpt Undo. Verificare che una
sequenza Draw -> selezione -> Undo ripristini la pennellata senza cambiare in modo inatteso il layer
selezionato.

**Vincolo:** Undo di una pennellata deve continuare a ripristinare il layer registrato corretto,
indipendentemente dal layer selezionato dopo la pennellata.

### P3 - Strength: commit solo quando esiste lavoro pendente

**Stato: PROPOSTA, priorita massima**

Condividere un controllo `has_pending_sculpt_transaction()` tra selezione e Strength. Eseguire il
commit soltanto quando esiste davvero una modifica pendente. Il primo evento Strength conserva
la semantica attuale; gli eventi successivi del medesimo trascinamento usano direttamente il
delta locale.

**Guadagno atteso:** da misurare; nessuna stima numerica prima del benchmark.

### P4 - Indice runtime grid -> layer payload e grid -> PBVH node

**Stato: PARZIALMENTE INTEGRATA**

Il primo indice e ora attivo per default. Resta da aggiungere il secondo:

- elenco ordinato delle griglie non vuote per il layer attivo: **integrato**;
- mappa da grid a nodo PBVH o lista dei nodi coperti dal layer.

Questo elimina due scansioni ripetute durante il trascinamento Strength. Gli indici non vanno
salvati nel `.blend`; devono rispettare un budget RAM e avere invalidazione centralizzata.

### P5 - Operatore Strength interattivo begin/update/end

**Stato: PROPOSTA**

Sostituire il solo campo Strength della sidebar con un operatore interattivo che:

1. al begin committa una sola volta e apre un'unica transazione Undo;
2. durante update applica delta locali e limita la frequenza degli aggiornamenti costosi;
3. all'end consolida valore, normali/bounds finali e notifica depsgraph;
4. con Esc ripristina esattamente valore e superficie iniziali.

Il valore deve restare animabile e modificabile via RNA/Python; il nuovo operatore riguarda solo
il trascinamento UI.

### P6 - Aggiornamento superficie a due qualita durante Strength

**Stato: PROPOSTA SPERIMENTALE**

Durante il trascinamento aggiornare posizioni e bounds necessari alla viewport, rinviando il lavoro
completo su normali/cuciture alla fine solo se una prova dimostra che l'anteprima non produce crack
o shading fuorviante. Non attivare per default senza test su bordi straordinari e UV seams.

### P7 - Lista layer virtualizzata

**Stato: PROPOSTA**

Sostituire il loop Python di tutte le righe con `UIList/template_list` o una finestra di righe
visibili. Conservare gruppi, collapse, visibilita e pulsanti correnti. Misurare separatamente:

- tempo di `draw()` con 10, 100, 500 layer;
- tempo click -> cambio indice;
- costo di rinomina e trascinamento Strength.

### P8 - Unificare selezione RNA e operatore

**Stato: PROPOSTA**

Creare una funzione C++ comune che esegua commit condizionale, cambio indice, REC policy e scelta
redraw/rebuild. RNA e operatore devono differire solo per report e gestione Undo. Questo e prima di
tutto un miglioramento di stabilita, ma impedisce regressioni prestazionali tra le due UI.

### P9 - Simmetria Multires per coppie di griglie

**Stato: INTEGRATA SPERIMENTALE**

L'operatore attuale visita entrambe le griglie di ogni coppia speculare. Per il lato destinazione
valuta anche la sorgente, mentre la successiva visita del lato sorgente esegue una terza valutazione
della limit surface prima di scartarlo. Elaborare ogni coppia `grid/corner_map[grid]` una sola volta:

- valutare i due campioni corrispondenti una sola volta;
- scegliere sorgente e destinazione dal segno rispetto al piano;
- mantenere staging transazionale, maschera, controlli di frame e pubblicazione finale invariati;
- gestire separatamente griglie sul piano e coppie auto-corrispondenti.

**Guadagno teorico sul nucleo:** circa 33% delle valutazioni limit-surface nel caso simmetrico
ordinario. Il guadagno end-to-end va misurato separatamente per Detail e Base.

Implementata per Active Detail il 2026-11-02: ogni coppia reciproca e posseduta dal grid con indice
minore, i due frame vengono valutati una sola volta per coppia/campione e riusati in entrambe le
direzioni eventualmente valide. Le griglie auto-corrispondenti conservano il percorso precedente.
Compilazione e startup test superati; equivalenza numerica e benchmark interattivo ancora richiesti.

### P10 - Dirty region per trasformazioni Move/Rotate/Scale

**Stato: PROPOSTA PRIORITARIA - REGRESSIONE OSSERVATA**

Dopo traslazioni o rotazioni di una parte della mesh, il commit puo diventare molto costoso anche
quando la regione modificata e piccola. Ipotesi da verificare con trace: l'operazione invalida CCG,
PBVH, normali o bounds sull'intera mesh e forza un percorso GLOBAL.

Intervento proposto:

- registrare durante G/R/S l'insieme di griglie e nodi PBVH realmente mossi;
- propagare dirty coords/normals/bounds soltanto ai nodi interessati e ai loro parent;
- distinguere trasformazione geometrica locale da cambio di topologia;
- usare GLOBAL solo quando il dominio dirty non e disponibile, la topologia cambia o la verifica
  di cucitura richiede esplicitamente l'intera superficie;
- aggiungere al trace `dirty_grids`, `dirty_nodes`, motivo del fallback e tempi CCG/PBVH separati.

Test obbligatori: piccola regione traslata, piccola regione ruotata, trasformazione oltre seam,
maschera parziale, Face Set, livello inferiore e Undo/Redo.

**Prima ispezione del percorso:** `apply_translations()` registra gia griglie e campioni con delta
non nullo e `multires_sculpt_changes_sync()` preferisce il journal esatto. Sono stati identificati
due fallback GLOBAL indipendenti dalla dimensione reale dei campioni mossi:

- `dirty_grids_over_half`: il locale viene rifiutato oltre il 50% delle griglie;
- `mask_dirty`: coordinate e maschera dirty insieme impediscono il commit locale.

Questi due motivi devono essere confermati con `LAYER_COMMIT_ROUTE` e `SCULPT_EXACT_GRIDS` prima di
cambiare policy. Candidato successivo: usare il numero di campioni esatti per la decisione e
scaricare separatamente la maschera prima del solve locale delle coordinate.

**Misura interattiva 2026-11-02, mesh 424456 griglie / area 289:** il caso osservato non ha usato
nessuno dei due fallback. Il journal esatto ha ristretto 22828 griglie PBVH a 22590 griglie reali
(5.32% del totale); 6482582 campioni su 6528510 erano effettivamente mossi. Il commit e rimasto
LOCAL e ha richiesto 395.439 ms complessivi (solve 236.371 ms, publish 112.258 ms). Quindi, in
questa prova, G/R/S e il commit non spiegano il blocco successivo.

Il costo dominante misurato e invece nel confine Polypaint/Undo:

- `POLYPAINT_LAZY_MASTER from=0 to=5` seguito da rebuild CCG globale: circa 10.1 s;
- decode memfile `Prepare Polypaint Resolution`: 25.9 s + 32.3 s, Undo totale 58.3 s;
- decode `Prepare Polypaint Attribute`: 47.9 s;
- decode `Before Polypaint Attribute Change`: 43.0 s;
- ogni decode provoca un nuovo rebuild CCG globale da circa 9.8 a 13.3 s.

Nuova priorita: evitare checkpoint memfile multipli per la preparazione lazy di risoluzione e
attributo Polypaint, conservando un solo confine Undo nativo/localizzato e impedendo rebuild CCG
ripetuti durante una singola sequenza Undo.

## Ottimizzazioni successive per le stesse feature

1. **Dirty generation per layer.** Un contatore cambia solo quando payload, topologia, livello o
   maschera pertinente cambiano. Cache e indici si validano confrontando la generation, evitando
   hash o scansioni integrali.
2. **Bounds incrementali reali.** Aggiornare esclusivamente i nodi PBVH toccati e i loro parent,
   verificando che `update_bounds_grids()` non ricalcoli nodi estranei.
3. **Batch per gruppi layer.** Visibilita e Strength di un gruppo dovrebbero comporre una sola
   regione, un solo aggiornamento normali e un solo passaggio Undo.
4. **Prewarm controllato.** Dopo la selezione, preparare in idle gli indici del layer attivo solo
   se la RAM disponibile supera la soglia dinamica. Nessun lavoro in background durante stroke.
5. **Contatori fallback.** Esporre in diagnostica quante operazioni usano LOCAL, METADATA o GLOBAL
   e il motivo del fallback. Una media veloce puo nascondere picchi globali sporadici.
6. **Budget temporale UI.** Se un update Strength supera il budget di un frame, accorpare gli eventi
   intermedi mantenendo sempre l'ultimo valore richiesto.
7. **Notifiche minime.** Verificare se `ND_MODIFIER` o `ID_RECALC_SYNC_TO_EVAL` causano lavoro non
   necessario nei percorsi metadata-only; preferire redraw locale quando la geometria e invariata.

## Protocollo benchmark obbligatorio

Usare la stessa mesh e lo stesso `.blend` per baseline e candidato. Registrare almeno:

| Scenario | Misura |
|---|---|
| Selezione senza modifiche pendenti | 20 alternanze tra due layer |
| Selezione dopo Draw | commit, rebuild, tempo totale |
| Selezione dopo maschera | preservazione maschera e tempo totale |
| Strength layer denso | 0 -> 1 -> 0 durante trascinamento |
| Strength layer sparso | stessa sequenza e numero di facce/nodi aggiornati |
| Livello inferiore | selezione e Strength sotto il livello massimo |
| Undo/Redo | contenuto layer, indice attivo, Strength e superficie |
| Salvataggio/riapertura | equivalenza numerica e visiva |

Abilitare `BLENDER_LAYER_RESP_TRACE=1` e raccogliere almeno:

- `LAYER_SELECT_OPERATOR_V03` / `LAYER_SELECT_V01`;
- `LAYER_STRENGTH_LOCAL`;
- `LAYER_REGION_V01`;
- `LAYER_WEIGHT_PROFILE`;
- `LAYER_COMMIT_ROUTE`;
- `LAYER_UNDO_ANCHOR` e percorso Undo usato.

Per ogni prova annotare mediana, massimo e numero di fallback globali. Il massimo e il numero di
fallback sono piu importanti della sola media per la fluidita percepita.

## Criteri di accettazione

- superficie e payload layer identici alla baseline entro la tolleranza numerica definita dal test;
- nessuna perdita o modifica della maschera;
- nessun crack tra grid o regressione delle normali;
- Undo/Redo coerente dopo selezione, Strength e cambio livello;
- salvataggio e riapertura equivalenti;
- memoria limitata da budget e rilasciata al cambio oggetto/file;
- nessun aumento dei fallback globali nei casi gia locali.

## Registro decisioni

| Data | Decisione | Esito |
|---|---|---|
| 2026-11-02 | Creazione del tracker dalla baseline `1eaaa7d9fe8b` | Fatto |
| 2026-11-02 | Verificato che selezione metadata-only e Strength locale esistono gia | Fatto; benchmark ancora mancante |
| 2026-11-02 | Priorita proposta: P3, poi P1/P2, P4, P7 | Da implementare dopo baseline misurata |
| 2026-11-02 | Rimossa la selezione layer dallo stack Undo globale | Integrata; test Draw/selezione/Undo richiesto |
| 2026-11-02 | Abilitato per default l'indice runtime delle griglie occupate | Integrata; A/B disponibile con env `=0` |
| 2026-11-02 | Rilevato commit costoso dopo traslazioni/rotazioni locali | Annotata P10; verificare invalidazione globale con trace |
| 2026-11-02 | Analizzato il nucleo Simmetria Multires | Annotata P9; evitare la terza valutazione per coppia speculare |
| 2026-11-02 | Simmetria Active Detail elaborata per coppie reciproche | Integrata sperimentale; build/startup superati, test interattivo richiesto |
| 2026-11-02 | Ispezionato commit dopo G/R/S | Journal esatto gia presente; candidati principali `dirty_grids_over_half` e `mask_dirty` |
| 2026-11-02 | Misurato G/R/S su 424456 griglie | Commit LOCAL 395.439 ms; blocco reale in memfile Undo Polypaint e rebuild CCG ripetuti |
| 2026-11-02 | Separato il pivot condiviso dalla geometria | `metadata_only`: niente CCG, notifier geometrico o secondo commit dopo Move |
| 2026-11-02 | Profilato primo commit su 24047 griglie | 161.8 ms cold contro 107.4 ms hot; circa 54 ms sono costruzione iniziale di 8192 frame cache |
| 2026-11-02 | Benchmark headless su `zombie dragon.blend` (3 GB) | Selezione mediana 0.0018 ms; Strength denso mediana 3291 ms, massimo 3478 ms |
| 2026-11-02 | Confronto build stabile/candidata | Strength stabile 3228 ms contro candidata 3291 ms: nessun guadagno dimostrato, differenza nel rumore |
| 2026-11-02 | Strength differito aggiunto alla UI | Pulsante orologio: modifica locale fluida, una sola valutazione esatta con OK; campo RNA nativo preservato come fallback |
| 2026-11-02 | Test interattivo cache progressiva | Regressione: primo commit 342.6 ms contro baseline 161.8 ms; disabilitata nel launcher, codice conservato opt-in |
| 2026-11-02 | Isolato costo Commit esplicito | Commit locale 9-375 ms, ma Undo globale 9.6-60.3 s e CCG 8-17 s; rimosso Undo operatore ridondante e forzati KEEP_CCG/DEFER_SYNC |
| 2026-11-02 | Strength sparso validato | Primo build 257 ms, successivi cache-hit 82-96 ms su 22964/424456 griglie |

## Ricognizione cold commit e reattivita UI (2026-11-02)

- Lo staging `FULL_GRID` osservato occupa circa 83 MiB, ma la sua allocazione misurata e circa
  0.03 ms. Con Move, 6.90 milioni di 6.95 milioni di campioni sono realmente risolti: il layout
  packed non puo ridurre significativamente ne lavoro ne memoria in questo caso.
- La cache frame e limitata sia dal budget RAM dinamico sia dal tetto predefinito di 8192 griglie.
  Sul commit osservato lascia 15855 griglie sul percorso generale. Una capacita adattiva maggiore
  puo accelerare commit successivi, ma aumenterebbe il costo del primo riempimento; non va quindi
  abilitata senza un criterio basato sul working set e sulla RAM disponibile.
- Candidato sicuro: mantenere la cache tra operazioni quando fingerprint, livello e topologia sono
  invariati, e verificare ogni reset inatteso con un motivo diagnostico esplicito.
- Candidato a rischio moderato: cache con crescita progressiva. Il primo commit costruisce solo il
  minimo necessario entro un budget temporale; i commit successivi ampliano il working set fino al
  limite RAM. Evita di trasformare il primo uso in un prewarm globale.
- Strength esegue ancora un `commit()` preventivo prima di conoscere se esistono coordinate sporche.
  Aggiungere un fast-path `NO_PENDING_COORDS` eviterebbe ingresso nel commit per trascinamenti dopo
  una superficie gia pubblicata.
- La selezione layer possiede gia il percorso draw-only e non crea Undo globale; resta da evitare
  la chiamata completa a `commit()` quando `ccg->dirty.coords` e falso, conservando i controlli di
  maschera e stato REC necessari.
- L'aggiornamento Strength sparso esiste ma e opt-in (`BLENDER_SCULPT_LAYER_REGION_V01=1`). Va
  validato e poi promosso a default: su layer realmente sparsi evita scansioni lineari di tutte le
  106114 facce.
- Lo skip dello stitching sui cache-hit Strength e ancora sperimentale. Prima di abilitarlo servono
  test numerici sui bordi delle griglie; non e candidato per una build stabile immediata.
- Il layer misurato occupa 424456/424456 griglie e 122667784 campioni. La cache object-space
  completa richiederebbe circa 1.4 GiB e viene correttamente esclusa dal budget dinamico di
  256 MiB. Non aumentare il budget predefinito per risolvere questo caso.
- Breakdown Strength denso misurato: circa 1.9 s trasformazione delta, 0.68-0.71 s
  stitching/normali e 0.63-0.65 s bounds PBVH. La regione sparsa non si applica correttamente a
  un layer con supporto 100%; il prossimo candidato e il coalescing degli eventi UI, mantenendo
  identico l'aggiornamento esatto finale.
# Native symmetry reference frame (implemented 2026-11-02)

- The movable symmetry reference is stored as a persistent object-local canonical frame.
- Its native RNA properties use `PROP_NO_DEG_UPDATE`: G/R/S metadata changes do not invalidate
  Multires, CCG, PBVH, or object geometry.
- Modal G/R/S remains viewport-only and writes the native frame once on confirmation.
- Existing files migrate transparently from the legacy Python property group on first use.
- Sculpt strokes use `reference_to_object * mirror * object_to_reference`; Multires detail
  symmetry converts the same frame to a stable object-local plane.
- Automated verification: dependency-graph update list remained empty after frame edits, and a
  save/reopen round-trip preserved location, rotation, size, and validity.

