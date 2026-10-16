# Sculpt Extended — ulteriori opportunità di ottimizzazione

16 ottobre 2026. Sorgenti del checkpoint `01194d089eb5b324b125d0ef6ebaf877dd13f326`; verificato nuovamente il riferimento remoto. `main`, prima di questa pubblicazione, era `208156f4602833df54710da252306dbeae01d8f0` e ospitava le note condivise. Questo documento non certifica patch o sorgenti locali Windows successivi al checkpoint.

Queste sono opportunità rilevate nel codice, non accelerazioni già integrate. Nessuna build nativa Blender e nessun benchmark di pennellate, Undo o Conform è stato eseguito. Rimangono validi i difetti B01–B11 dei report precedenti e la necessità di conservare tutte le ottimizzazioni già funzionanti e il master HR indipendente dal livello di lavoro.

## Ordine proposto

| ID | Intervento | Vantaggio cercato | Evidenza e limite |
|---|---|---|---|
| O07 | Specializzare il trasferimento Polypaint quando paint level = master level | Meno calcolo e scritture per campione dipinto | Kernel reale confrontato con variante sperimentale; compatibilità IEEE da definire |
| O08 | Staging Undo decodificato entro un budget | Evitare la seconda decompressione nei piccoli stroke | Due passaggi presenti nel corpo reale; nessuna misura del codec o del tempo Undo |
| O09 | Conform: riconoscere pesi tutti nulli, riusare soltanto la topologia stabile | Evitare preparazioni inutili e ricostruzioni strutturali | Percorso sorgente completo fino al backend; va preservata la semantica di ritorno e delle seam |
| O02a | Indice sparse del supporto con più voci | Evitare ricostruzioni alternando layer A/B | Il support index è singleton, la cache dei delta ha già un pool: non confonderli |
| O06b | Resample per griglie con parallelismo limitato dalla RAM | Usare più core senza ricreare copie integrali dei layer | Il percorso conserva già staging compatto e tabelle coordinate; il guadagno dipende dalla banda memoria |

O02a e O06b approfondiscono O02/O06: non sono presentati come ottimizzazioni interamente nuove. Per l'uso quotidiano restano prioritari anche O01 (Delta Smooth locale) e O05/O05a (Undo Face Set uniforme), già documentati e non rimisurati qui.

## O07 — Polypaint: caso paint = master e pesi nulli

[Kernel `add_coarse_delta`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/polypaint_cpu_math.hh#L58-L103), [writer](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L1318-L1357).

La funzione applica un delta a supporto bilineare. Quando `master_side == paint_side`, il raggio è 1: per un campione interno attraversa 3×3 celle, ma otto hanno peso zero. Il kernel effettua comunque le quattro addizioni RGBA su ciascuna cella. Ai bordi il numero di celle è inferiore. Il writer ricava anche il colore corrente tramite interpolazione bilineare pur trovandosi esattamente su un campione del master.

Possibile specializzazione: lettura diretta del campione e aggiornamento del solo centro quando i livelli coincidono. Per il trasferimento coarse → HR, escludere dal loop i bordi a peso nullo o saltarne le scritture; conservare ordine degli aggiornamenti e residuo HR. Non abbassare il livello del master e non raggruppare delta in modo da alterare l'ordine di arrotondamento.

Prova effettuata: header matematico originale contro variante che mantiene formule e ordine dei cicli, saltando soltanto `w == 0`. Sono state confrontate bit per bit **2.800 operazioni sequenziali**, con side 2–65, tutte le 28 coppie master/paint ammesse nella selezione, coordinate pseudo-casuali incluse quelle di bordo e colori iniziali positivi finiti. Nessuna divergenza in quel dominio. I conteggi geometrici per campioni interni sono:

| Rapporto degli intervalli master/paint | Celle visitate | Celle con peso non nullo |
|---|---:|---:|
| 1 | 9 | 1 |
| 2 | 25 | 9 |
| 4 | 81 | 49 |
| 8 | 289 | 225 |

Questo non dimostra un'accelerazione 9× dell'intera pennellata. Lookup, Undo, allocazioni e pubblicazione del colore possono dominare il tempo.

**Controesempio intenzionale:** su un vicino con valore `-0.0`, il codice originale aggiunge `+0.0` e produce `+0.0`; saltare la scrittura conserva `-0.0`. Pertanto la variante non è universalmente identica bit per bit. Il colore numerico coincide, ma bisogna scegliere e documentare il contratto. Vanno inoltre provati colori negativi/HDR, buffer non finiti, alpha, seam, simmetria, Undo/Redo e il writer completo. La lettura diretta nel writer non è stata implementata né confrontata in questo test.

## O08 — Undo: due decompressioni per transazione

[`sculpt_layer_undo_persistent_payload_swap`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L1432-L1513) decomprime indici e delta nel passaggio di validazione, poi li decomprime ancora nel passaggio di scambio e ricomprime i dati Redo.

La validazione completa prima della mutazione è un requisito utile. Non rimuoverla per velocizzare Undo. Proposta: conservare i blocchi decodificati durante il preflight solo se l'intera transazione rientra in un budget dichiarato; usare il percorso attuale oltre soglia. Il risparmio riguarda una decompressione di indici e delta per nodo ammesso nello staging, non dimezza necessariamente Undo: scambio, ricompressione e aggiornamento della superficie restano.

Il costo è RAM temporanea aggiuntiva per tutti i blocchi trattenuti. Non creare uno staging illimitato: l'attuale riuso di un buffer per volta contiene proprio quel picco. Misurare byte compressi/decodificati, tempo di decode/encode, RSS e p95 Undo/Redo. Un eventuale scambio parallelo richiederebbe anche una garanzia esplicita di griglie disgiunte; non è parte della proposta verificata.

## O09 — Conform: no-op e topologia temporanea

[Preparazione CCG](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L479-L699), [backend](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/shrinkwrap.cc#L1329-L1391), [caller](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/modifiers/intern/MOD_multires.cc#L671-L680).

La forza globale nulla è **già esclusa** da `shrinkwrap_enabled`: non proporla come nuovo fast path. Restano i pesi effettivi tutti nulli per maschera o gruppo. Attualmente si preparano pesi, eventualmente normali e mesh di proiezione, poi il backend risolve il target e inizializza l'albero; i callback scartano soltanto allora i singoli campioni con peso zero.

Un test/reduction dei pesi, preferibilmente fuso con la loro produzione, potrebbe evitare normali, proiezione e chiamata al backend in questo caso. Attenzione: il backend ritorna `true` dopo dispatch anche se non è stato spostato alcun campione. Il caller può quindi eseguire averaging delle griglie e ricalcolo delle normali. Un nuovo `return false` anticipato cambierebbe quel comportamento. Prima separare semanticamente successo, modifica delle posizioni e necessità di aggiornare le seam; oppure preservare esplicitamente il comportamento del caller. Questa ottimizzazione è una proposta statica, non un test nativo di equivalenza.

Per Project sulle normali con Subdivision Levels > 0, `shrinkwrap_grid_projection_positions` costruisce mappe di saldatura logica, indici compatti, facce e spigoli a ogni chiamata. Proposta distinta: riusare la sola struttura topologica finché identità/topologia e livello CCG coincidono. Posizioni, normali, crease e risultati proiettati vanno aggiornati secondo i rispettivi input. Mai riusare normali solo perché la topologia è immutata; mai saldare per vicinanza spaziale. Non aggiungere una cache senza budget: sposterebbe RAM temporanea in RAM persistente, mentre il resto di Conform evita già la mesh HR nei metodi che non la richiedono.

## O02a — Evitare thrashing dell'indice sparse tra layer

[`LayerPayloadIndex`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L72-L165) conserva una sola voce. Con support cache abilitata, una sequenza A → B → A di layer distinti ricostruisce l'indice tre volte anche senza modifiche. La cache dei delta trasformati in `weight_direct_delta` possiede già un pool LRU; estenderla nuovamente non risolve questo indice separato.

Proposta: poche voci del support index con budget e invalidazione per layer, oppure integrare il supporto nelle voci della cache esistente. Conservare le verifiche dei descrittori e l'invalidazione quando un payload sparse viene creato/eliminato. Non usare soltanto l'indirizzo del layer come identità durevole dopo Undo o riciclo memoria. È una specializzazione di O02, soprattutto utile quando si alternano più layer; per uno slider ripetuto sullo stesso layer l'indice abilitato è già HIT. Con cache disabilitata il codice scansiona comunque i descrittori.

## O06b — Parallelizzare il resample entro il picco RAM

[`resample_all`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L1558-L1683) ha già tabelle coordinate per layer e sostituisce una griglia alla volta. Non reintrodurre due copie complete del layer. I cicli sui corner sono seriali e le griglie sono destinazioni indipendenti dopo il preflight globale.

Proposta: elaborare poche griglie in parallelo, con concorrenza derivata da budget temporaneo e dimensione della griglia di output, non dal solo numero di core. Ogni worker mantiene al massimo la propria griglia nuova; conservare validazione globale prima della mutazione e ownership dei buffer. Il budget temporaneo non elimina l'aumento permanente dei payload quando cresce il livello. Per griglie molto grandi può essere corretto mantenere un solo worker.

Resta un'ottimizzazione del percorso descritto, non una certificazione della sua compatibilità sparse: il preflight attuale rifiuta payload nulli. Le garanzie di correttezza vanno risolte prima di estendere il percorso. Riutilizzare le tabelle per coppia di livelli tra layer è possibile ma secondario rispetto al volume dei campioni. Misurare throughput, RSS e tempo validation/resample già esposto dal profiling; niente promesse di crescita lineare con i core.

## Riproducibilità e accettazione

`python verify_optimization_candidates.py` compila C++17 con GCC, AddressSanitizer e UndefinedBehaviorSanitizer e include l'header matematico originale. Output e manifest sono allegati. LeakSanitizer è disabilitato come nelle prove precedenti dell'ambiente. Nessuna diagnostica ASan/UBSan nel confronto completato; non certifica chiamanti, threading o pipeline Blender.

Gli altri percorsi sono stati ispezionati staticamente. Non sono state misurate latenze né applicati fix ai sorgenti Blender. Per accettare un intervento: stessa scena e build Release, controlli attivi/disattivi, cache fredda/calda, stesso stroke registrato, p50/p95 e picco RSS; confrontare layer, master HR, seam e Undo/Redo. Conservare i percorsi precedenti come controllo e fallback finché l'equivalenza richiesta non è dimostrata.
