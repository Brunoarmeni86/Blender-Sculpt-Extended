# Audit Sculpt Extended — 16 ottobre 2026

**Seguito:** [approfondimento con ulteriori riscontri, chiamanti e riproduzioni](DEEP-REVIEW.md).

## Versione, ambito e limiti

Revisione richiesta da Bruno per raccogliere difetti e ottimizzazioni in un riferimento GitHub condivisibile tra chat.

- **Sorgente principale:** `checkpoint/sculpt-extended-20261015`, commit [`01194d089eb5b324b125d0ef6ebaf877dd13f326`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/commit/01194d089eb5b324b125d0ef6ebaf877dd13f326), verificato sul remoto il 16 ottobre.
- **Main al momento dell'audit:** `cea35a4f31f575c3f1d4a3eb767b11e56707db7b`. I corpi di `merge_down`, `group_merge`, `flatten` e `faceset_resample_up` sono identici nei due commit.
- **Ramo Delta Smooth:** `experiment/delta-smooth-coarse-v01` a `1d5f69b0e66ed5e0bf3d18b25dfd25e4bca455f2`, divergente dal checkpoint (base comune `7b54ef82790a`). Il checkpoint contiene già modifiche Coarse V02 successive: non sostituirle automaticamente con il vecchio esperimento.
- Lettura mirata di storage layer/Face Sets/Polypaint, fusioni, Commit locale, cache, subdivision staging, Delta Smooth, acquisizione/ripristino Undo, bake UV e chiamanti RNA/operatori. Non è una revisione completa di Blender, Cycles o di tutti i pennelli.
- Le patch Face Sets HR più recenti distribuite separatamente nelle altre chat non sono state identificate come nuovi commit nel ramo esaminato. I risultati valgono per i commit sopra, non certificano quei kit o l'eseguibile locale dell'utente.
- **Nessuna correzione al codice applicata da questo audit.** Sono pubblicate documentazione e riproduzioni.

## Riscontri e priorità

P1 = correggere prima di usare in produzione l'operazione interessata. P2 = robustezza o ottimizzazione da programmare. Le priorità non implicano che il difetto si presenti in ogni sessione.

| ID | Priorità | Riscontro | Evidenza |
|---|---|---|---|
| B01 | P1 | Flatten elimina contributi su griglie sparse del primo layer | Corpo reale estratto, caso R4; controllo denso C1 |
| B02 | P1 | Merge Down sposta la selezione sul layer successivo; Merge Group può fermarsi prima o fallire dopo una modifica | Corpi reali estratti, casi R1–R3 |
| B03 | P1 | La stima RAM Undo omette il buffer HR dei Face Set | Corpo reale estratto, caso R7 |
| H01 | P2 | Resample Face Set non transazionale su input incompatibile | Corpo reale estratto, caso R5; input esplicitamente malformato |
| H02 | P2 | Merge accetta overflow dei delta HR e restituisce successo | Corpo reale estratto, caso R6; valori estremi finiti |
| K01 | P1 da verificare | Possibile inizializzazione concorrente del master Face Set da Undo | Percorso parallelo individuato; nessun crash/race nativo riprodotto |
| K02 | P1 da verificare | Chiave cache non comprende impostazioni/attributi che cambiano il limite Subdiv | Incompletezza della chiave visibile; effetto nativo da isolare |

### B01 — Flatten perde i contributi sparse

[Sorgente: `flatten`, righe 2946–3015](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2946-L3015).

Una griglia con `disps == nullptr` è un contributo zero valido. Durante l'accumulo nel primo layer, `flatten()` salta la griglia se il **destinatario** non ha payload; poi elimina comunque il layer sorgente. Esempio: A vuoto, B con dettaglio 7 sulla stessa griglia → Flatten lascia A vuoto ed elimina B. `compatible()` accetta la rappresentazione sparse, quindi il controllo dell'operatore non impedisce questo caso. Il controllo su layer nascosti nell'operatore esiste: non viene segnalato come ulteriore bug.

**Correzione proposta:** validare prima tutte le griglie; materializzare il destinatario soltanto dove esiste un contributo da sommare, o trasferire un payload quando consentito. Pubblicare e cancellare le sorgenti solo dopo il successo. Conservare il comportamento con strength diverse da 1 e la protezione dei layer nascosti.

**Test Blender da aggiungere:** primo layer vuoto/secondo scolpito; payload su griglie disgiunte; confronto superficie prima/dopo; Undo/Redo e save/reopen. Usare una copia del file.

### B02 — Indice attivo errato dopo Merge Down, con effetti su Merge Group

[Sorgente: `merge_down`, righe 2826–2944](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2826-L2944), [`group_merge`, righe 2613–2721](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2613-L2721), [chiamante operatore](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_ops.cc#L1563-L1585).

`dst_index` viene calcolato prima di scollegare `src`, che precede `dst`. Dopo la rimozione, il destinatario ha indice `dst_index - 1`. Con A/B/C, fusione A→B produce correttamente B=A+B ma attiva C. Il clamp in `active()` nasconde il difetto quando il destinatario è l'ultimo layer; non lo risolve in generale.

`group_merge()` presume che resti attivo il destinatario:

- gruppo A/B/C come ultimi tre layer: può restituire successo lasciando due layer e rimuovendo il gruppo soltanto da C;
- gruppo A/B seguito da C esterno: fonde A/B, poi restituisce errore perché il layer attivo è C. L'operatore risponde `CANCELLED` dopo una modifica persistente; il ripristino tramite Undo in questo caso non è stato verificato.

**Correzione proposta:** ricalcolare l'indice di `dst` dopo `BLI_remlink` oppure mantenere esplicitamente la sua identità. Rendere anche la fusione gruppo indipendente da indici mutabili e impedire failure dopo la pubblicazione senza rollback. Verificare gruppi in inizio/centro/fine lista, 2/3/4 membri e layer esterni adiacenti.

### B03 — RAM Undo Face Set HR non conteggiata

[Allocazione `grid_face_sets`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L2867-L2893), [stima `node_size_in_bytes`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L3477-L3496), [conteggio degli step](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L3520-L3532).

Gli snapshot Face Set allocano `numero_griglie * master_area * sizeof(int)` byte. `node_size_in_bytes()` include `face_sets` sulla base, ma non `grid_face_sets`. La stima passa a `undo_size` e quindi a `step.data_size`: il buffer HR sfugge al limite di memoria Undo. Nel test, aggiungere realmente 4 MiB al buffer produce **zero byte** di incremento nella stima.

**Correzione proposta:** conteggiare `grid_face_sets` e il bitset `grid_color_present`, controllare gli altri campi aggiunti e usare aritmetica a 64 bit nei prodotti. Non duplicare il conteggio dei delta Position già gestiti dalla compressione. Testare la variazione del conteggio con payload effettivi e poi il limite Undo nativo.

### H01 — Resample Face Set può lasciare livelli misti

[Sorgente, righe 1438–1478](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L1438-L1478).

La funzione controlla e sostituisce una griglia alla volta. Se una griglia successiva ha metadata incompatibili, restituisce `false` dopo aver già sostituito le precedenti; `sculpt_face_set_level` resta al valore vecchio. Inoltre le chiamate nella suddivisione ignorano il valore restituito ([esempio](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_reshape.cc#L700-L706)).

**Limite:** R5 introduce volutamente un descrittore malformato. Non dimostra che l'editing normale generi quel descrittore.

**Correzione proposta:** prevalidare tutte le griglie prima della prima mutazione; coordinare il successo con la pubblicazione della suddivisione e propagare gli errori. Usare staging dove rimangono failure recuperabili.

### H02 — Overflow numerico non respinto nelle fusioni HR

In `merge_down`, i delta L0 passano da `base_math::compose`, che controlla finitezza e risultato; l'accumulo HR usa moltiplicazioni/somme dirette senza lo stesso controllo. Due valori finiti estremi producono `Inf`, la sorgente viene eliminata e la funzione ritorna `true`.

**Limite:** R6 usa valori prossimi a `FLT_MAX`, non uno sculpt ordinario. Non dimostra la causa degli spike segnalati in precedenza.

**Correzione proposta:** prevalidare input e risultati prima di pubblicare; rifiutare overflow conservando i dati originali. Estendere il controllo anche a Flatten e fusione gruppo, senza aggiungere una scansione dell'intera mesh a ogni pennellata.

## Percorsi da verificare nell'integrazione Blender

### K01 — Inizializzazione master Face Set nei worker Undo

[`push_nodes` esegue il riempimento in parallelo](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L3082-L3102). `fill_node_data_grids(Type::FaceSet)` chiama `faceset_ensure_from_mesh`, che può allocare o ricampionare storage condiviso del modifier senza sincronizzazione. Il mutex di `ensure_node` protegge la directory degli Undo node, non questa inizializzazione. Anche [`face_sets_update`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_face_set.cc#L329-L350) chiama `push_node` nei worker.

**Condizione da esercitare:** prima operazione Face Set su Multires con almeno due PBVH node e master assente o a livello inferiore. Non è stata riprodotta una race nell'eseguibile. Preparare il master una sola volta nel tratto seriale precedente alla parallelizzazione; lasciare ai worker soltanto copia/lettura di storage stabile. Verificare anche la promozione Polypaint prima degli snapshot Color.

### K02 — Validità cache rispetto all'evaluatore Subdiv

[`multires_sculpt_commit_base_fingerprint`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires.cc#L1063-L1100) include posizioni e connettività della cage, ma non crease, boundary mode, quality o una revisione dell'evaluatore. Le cache frame e object-delta riutilizzano questa chiave. Cambiare il limite Subdiv a cage invariata può quindi richiedere invalidazione aggiuntiva; va verificata l'intera catena dei callback prima di dichiarare una regressione riprodotta.

**Prova proposta:** riscaldare cache, cambiare crease/boundary/quality senza muovere vertici, confrontare cache ON/OFF e valutazione completa, per Commit e strength/visibility. Una revisione esplicita che copra tutti gli input dell'evaluatore è preferibile al solo puntatore temporaneo. Il launcher completo esaminato disabilita la frame cache, ma abilita la multi-cache dei pesi: i due casi vanno distinti.

## Ottimizzazioni concrete da misurare

Nessuna percentuale di accelerazione viene promessa: qui sono identificati lavoro e memoria evitabili, senza benchmark dell'eseguibile.

| ID | Punto del codice | Proposta | Verifica necessaria |
|---|---|---|---|
| O01 | `layer_delta_smooth_v01::prepare_region/raw_offset` | Catturare soltanto campioni necessari e alone dei vicini; batch delle valutazioni di tangente; storage contiguo per griglia o tile invece di due mappe per campione | Primo dab, stroke lungo, livelli inferiori, seam, simmetria e Auto Smooth; confrontare delta finali |
| O02 | `weight_direct_delta`, fingerprint e support index | Evitare a ogni tick scansioni/layout temporanei ripetuti: contatore revisione completo, supporto per layer e riuso dei working buffer | Costi HIT/BUILD separati; invalidazione su Undo/topologia/crease/Conform/deformazioni |
| O03 | Commit locale, exact sample journal | Tenere conteggi/lista o bitset a blocchi dei campioni dirty per non ricontare tutta l'area di ogni griglia, conservando fallback e gestione duplicati | Stroke sottili contro griglie quasi piene; stesso footprint e stessi valori pubblicati |
| O04 | Bake UV Polypaint e `polypaint_sample_with_base` | Risolvere master/attributo una volta, preparare una patch colore per corner, riusare l'inversa affine dove possibile, rasterizzare per tile con ownership deterministica | UV specchiate, sovrapposte, degenerate, bordi patch, alpha, UDIM; confronto numerico |
| O05 | Snapshot/ripristino Face Set e Color | Salvare presenza/uniformità e tile modificati; preservare master HR; non materializzare tutte le griglie uniformi durante Undo | Identità dei campioni, semantica base-fallback, Undo/Redo, memoria prima/dopo |
| O06 | Suddivisione layer | Proseguire staging compatto già presente; misurare copie attributi/maschere e costo delle superfici temporanee; condividere solo dati realmente immutabili | Picco RSS per fase, layer sparse/densi, dettagli e seam, annullamento/fallimento |

Riferimenti:

- O01: [`prepare_region`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_layer_delta_smooth_v01.hh#L211-L243) attraversa ogni campione dei nodi e memorizza riferimenti/offset prima dell'applicazione dei fattori; i calcoli della reference sono seriali. Un campione può appartenere a una griglia selezionata senza essere dentro il pennello.
- O02: [`weight_direct_delta`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires.cc#L1203-L1349) ricrea contesto, fingerprint, maschere e lookup anche quando la cache object-delta è HIT. Correggere prima K02 se si cambia l'invalidazione.
- O03: [conteggio exact samples](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires.cc#L1508-L1568) esamina ogni bit di ogni griglia dirty prima del solve ridotto.
- O04: [loop bake](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_polypaint_uv_bake_v01.hh#L113-L159), [fallback patch colore](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L1149-L1240): nel fallback l'attributo e il centro della faccia vengono ricavati ripetutamente per campione.
- O05: [`restore_face_sets_grids`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L1099-L1132) chiama `faceset_grid_ensure_dense` anche per uno snapshot uniforme. Gli Undo Face Set non mantengono la rappresentazione uniforme come fanno i presence bit Polypaint.
- O06: [`multires_subdivide_compact_layers`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_reshape.cc#L611-L716) esiste già: non proporre come nuovo un algoritmo già integrato.

**Dimensioni utili per interpretare la memoria:** per G griglie catturate, A campioni nel master per griglia, il solo snapshot colore costa `16*G*A` byte, quello Face Set `4*G*A`. A livello 9 il lato è 257: 1.000 griglie significano circa **1.008 MiB colore** o **252 MiB Face Set**, esclusi gli altri buffer. Sono calcoli di dimensionamento, non misure di una sessione. Abbassare lo Sculpt Level non abbassa automaticamente questa memoria perché il master resta HR, come richiesto dal progetto.

## Verifiche eseguite e riproducibilità

[Script](verify_findings.py), [risultati](verification-results.txt), [manifest funzioni](verification-manifest.json), [blob originali](remote-source-manifest.json).

Otto verifiche su corpi di produzione estratti: sette osservazioni dei difetti/robustezza R1–R7 e un controllo denso C1. Sono usati adattatori minimi di storage, liste, Array e API; il kernel `base_math` è quello originale. Questi test verificano le funzioni, non depsgraph, PBVH, operator dispatch, Undo completo o serializzazione Blender.

Compilazione C++17 con `-Wall -Wextra -Werror -fsanitize=address,undefined`. Le asserzioni sulle manifestazioni attese passano, senza diagnostiche ASan/UBSan. LeakSanitizer è disabilitato (`detect_leaks=0`) perché l'executor non può enumerare `/proc` per il controllo dei leak. Nessuna build completa Blender o prova GUI è stata eseguita.

I tre file sorgente usati dai test sono stati controllati byte per byte tramite il Git blob SHA del remoto. Per rieseguire da un checkout del commit esaminato, con Python 3 e g++ disponibili:

```sh
python /percorso/delle/note/verify_findings.py /percorso/checkout-audited
```

Lo script estrae le funzioni dal checkout indicato e scrive i risultati accanto a sé. Rieseguendolo su una versione diversa, confrontare i commit e non presumere che i difetti siano ancora presenti. Non è un test che pretende una build sana: le asserzioni documentano il comportamento difettoso della versione esaminata.

## Ordine di lavoro suggerito alle altre chat

1. Correggere B01 e B02 in un ramo dedicato, verificando conservazione della superficie e delle identità layer.
2. Correggere B03 e confrontare Undo RAM stimata/reale su Face Set HR; verificare K01 prima di nuove parallelizzazioni.
3. Chiudere H01/H02 con validazione prima della pubblicazione; testare i percorsi d'errore.
4. Riprodurre o escludere K02; poi misurare O01/O02 sul modello reale e con configurazione identica.
5. Proseguire O03–O06 per piccoli interventi misurabili, conservando i master HR e il Commit detail-preserving ai livelli inferiori.

Per ogni fix registrare commit, test e stato dell'ID. Non segnare un riscontro come risolto soltanto perché è stata generata una patch. Non sostituire una versione più recente con quella dell'audit.
