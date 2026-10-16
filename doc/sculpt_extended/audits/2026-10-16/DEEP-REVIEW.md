# Approfondimento dell'audit — 16 ottobre 2026

**Seguito:** [terzo approfondimento — animazione, gruppi e identità dopo Edit](ANIMATION-TOPOLOGY-REVIEW.md).

Segue il [primo report](README.md). Rimane valido il riferimento `01194d089eb5b324b125d0ef6ebaf877dd13f326`, checkpoint `checkpoint/sculpt-extended-20261015`. Il ramo è stato ricontrollato prima dell'approfondimento: non è cambiato. Main contiene la prima pubblicazione delle note, non nuove correzioni al codice.

Questo seguito approfondisce chiamanti, trasformazioni, conteggio Undo e lettura/scrittura sparse. I risultati precedenti restano aperti. **Nessun fix al codice applicato.** Non è una certificazione delle successive patch HR Face Sets distribuite fuori da questi commit.

## Nuovi riscontri

| ID | Priorità | Riscontro | Livello di verifica |
|---|---|---|---|
| B04 | P1 | Il vecchio Bake Visible into Base dereferenzia un payload sparse nullo | Corpo reale; errore UBSan; chiamante e pulsante identificati |
| B05 | P1 | Apply Scale scala il displacement nativo, lasciando invariati delta HR e L0 dei layer | Corpo reale; controllo dati + invariante geometrico piano; flusso GUI da testare |
| B06 | P1 | Il limite Undo può leggere una dimensione registrata prima della fine della compressione | Corpi reali encode/getter, completamento ritardato simulato; chiamante del limite letto |
| H03 | P2 | Restore Face Set usa buffer esistente senza controllarne livello/dimensione | Heap-buffer-overflow ASan su descrittore volutamente malformato |
| H04 | P2 | Overflow signed nel calcolo della soglia dense-sample a livello 12 | Espressione estratta; UBSan; controllo livello 11 |
| O05a | Ottimizzazione confermata | Undo Face Set invariato trasforma storage uniforme in denso | Corpo reale; risultato `modified=false` con nuova allocazione |
| K03 | P1 da verificare | Join non mostra una migrazione esplicita dei master memorizzati nel modifier | Catena di chiamate letta; nessun Join nativo eseguito |

P1 riguarda il percorso descritto: non implica che ogni apertura del file o pennellata lo attraversi. I test usano adattatori minimi e non sostituiscono l'eseguibile Blender.

## B04 — Vecchio Bake Visible into Base e storage sparse

[`bake_visible_to_base`, righe 1798–1850](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L1798-L1850) legge e azzera direttamente `layer.disps[corner].disps[i][axis]`. Non controlla il puntatore di payload, pur accettando `compatible()`, che consente correttamente i payload nulli come contributi zero.

La riproduzione crea metadata accettati da `compatible`, base nativa allocata e layer sparse vuoto. Il corpo estratto segnala `runtime error: load of null pointer of type 'float'`. Il controllo con layer denso conserva e azzera correttamente il contributo. Non viene allocata una mesh Blender completa nell'adattatore.

Il percorso è raggiungibile dal codice UI esaminato:

1. [Pulsante del modifier `Bake Visible into Base...`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/modifiers/intern/MOD_multires.cc#L898-L903).
2. Operatore [`SCULPT_OT_multires_base_capture`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_ops.cc#L793-L821).
3. `bake_visible_to_base()`.

**Distinzione importante per i fix:** questo è il vecchio `multires_base_capture`, diverso dal nuovo `SCULPT_OT_multires_merge_visible_to_base` / `merge_visible_to_base`. Quest'ultimo contiene già controlli sparse e rifiuta delta L0 effettivi che richiedono rebasing. Non trasferire automaticamente le conclusioni da un comando all'altro.

**Intervento proposto:** eliminare l'ambiguità tra i due comandi e condividere una sola implementazione con validazione, lettura sparse e pubblicazione sicure. Finché non è implementato il rebasing L0, rifiutarne esplicitamente i casi non preservabili. Un semplice inoltro al nuovo comando non copre automaticamente tutti i vecchi casi L0. Non risolvere materializzando indiscriminatamente ogni griglia di ogni layer.

**Test nativo mirato:** nuovo layer vuoto, layer parzialmente scolpito con altre griglie nulle, strength 0/0.5/1, presenza di delta L0, Undo/Redo e save/reopen. Controllare sia il pannello sia F3, usando copie dei file.

## B05 — Apply Scale omette i delta dei layer

[`object_transform.cc`, righe 865–874](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/object/object_transform.cc#L865-L874) chiama `multiresModifier_scale_disp()` e poi trasforma la mesh. La routine [`multires_apply_uniform_scale`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires.cc#L3028-L3040) moltiplica soltanto i campioni `CD_MDISPS` della mesh. Non attraversa `sculpt_layers` o `base_deltas`. La trasformazione mesh riceve la Mesh, non i payload appartenenti al modifier.

Nel test estratto con fattore 2:

| Storage | Prima | Dopo la routine | Necessario per scala uniforme positiva 2 |
|---|---:|---:|---:|
| Native MDisps | 1 | 2 | 2 |
| Delta HR layer | 1 | 1 | 2 |
| Delta L0 layer | 1 | 1 | 2 |

Gli assi del frame tangent sono [normalizzati esplicitamente](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_inline.hh#L19-L50), quindi non compensano automaticamente l'ampiezza mancante. Nel caso piano, senza L0 e con strength 1: prima, scala oggetto 2 × (base 1 + layer 1) = 4; dopo, scala applicata 1 × (base 2 + layer 1) = 3. Questo è un controllo analitico con routine reale, non una misura del viewport.

**Intervento proposto:** per il primo fix limitarsi alla scala uniforme positiva, preservare le griglie nulle, moltiplicare delta HR e L0 di tutti i layer, inclusi quelli nascosti, e invalidare le cache. La scala non uniforme e le riflessioni richiedono un controllo separato dei frame: non è sufficiente copiare il fattore scalare esistente. Valutare anche geometria condivisa e modifier multipli prima della pubblicazione.

**Test nativo:** confronto world-space prima/dopo Ctrl+A Scale, con native e layer separati; layer visibili/nascosti, L0, pesi diversi, Undo/Redo e riapertura. Nessuna conclusione viene attribuita qui ai precedenti spike dell'utente.

## B06 — Dimensione Undo registrata prima della compressione

La catena osservata è:

1. [`PositionUndoStorage`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L481-L520) avvia un task di compressione in background.
2. [`step_encode`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L3619-L3645) copia `undo_size` in `step.data_size` senza attendere quel task.
3. Il [task completato](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L580-L594) incrementa `owner_step_data->undo_size`, ma non aggiorna `UndoStep::data_size`.
4. Il [limite della cronologia](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/undo_system.cc#L426-L452) somma `us->data_size`.
5. Il [getter Sculpt](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L609-L622), usato dal [conteggio totale mostrato](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/undo/ed_undo.cc#L1209-L1223), attende invece il completamento e legge `undo_size`.

Nel test, i corpi reali di encode/getter vengono eseguiti in una sequenza con task ancora pendente; il completamento viene simulato nell'adattatore. Dopo il completamento: `undo_size = 1 MiB`, `step.data_size = 0`. Nessuna race di thread è dichiarata riprodotta: è verificato il mancato aggiornamento in quella sequenza. Non è stato ricostruito lo storico del difetto rispetto all'upstream.

**Effetto da verificare nativamente:** la memoria riportata dalla UI può differire da quella usata dal limite di cronologia. È distinto da B03, che omette proprio un buffer HR anche con conteggio sincrono.

**Intervento proposto:** definire un unico contratto di conteggio con sincronizzazione esplicita. Possibili soluzioni: prenotazione conservativa durante la compressione e conguaglio sul thread principale; oppure raccolta delle dimensioni completate prima del trimming, senza una lettura concorrente non protetta. Evitare di annullare il beneficio del background imponendo un'attesa completa su ogni stroke. Verificare anche le variazioni di dimensione dei buffer ricompressi durante Undo/Redo e il conteggio dei metadata.

## H03 — Buffer Face Set non validato nel restore

[`faceset_grid_ensure_dense`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L1411-L1436) valida il master globale, ma se `grid.ids` è già presente lo restituisce senza controllare `grid.level` e `grid.totid`. [`restore_face_sets_grids`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc#L1099-L1132) percorre poi l'intera area dello snapshot.

Il test imposta volutamente un buffer da 1 int dove il master attende 4. ASan rileva una lettura oltre il buffer. **Non è dimostrato che un file valido o una normale operazione producano questo stato.** Il caso giustifica un rifiuto sicuro prima del restore; non una dichiarazione di crash generale.

**Intervento proposto:** applicare al writer Face Set il controllo metadata già presente nel writer Polypaint, validare tutti i grid index e tutti i buffer prima di scambiare i dati, poi propagare il fallimento senza ripristini parziali. Riesaminare insieme H01 e l'inizializzazione concorrente K01.

## H04 — Soglia dense-sample: overflow a livello 12

[`multires.cc:1508`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires.cc#L1506-L1509) calcola:

```cpp
const int dense_sample_threshold = (area * 975 + 999) / 1000;
```

Con livello 12, lato 2049, `area = 4.198.401`: il prodotto supera il massimo di un int a 32 bit. UBSan rileva signed integer overflow sull'espressione originale. Il controllo livello 11 passa. Il calcolo è eseguito prima del controllo d'uso della frame cache, quindi spegnere quella cache non elimina da solo l'espressione.

**Limite pratico:** il test non alloca la corrispondente mesh e non dimostra un problema nei consueti livelli 4–6. Il range del codice accetta però livelli fino a 13.

**Intervento proposto:** effettuare il prodotto in `int64_t`, verificare il risultato e ricontrollare gli altri prodotti grid-count × area negli snapshot, negli indici e nelle cache. Non imporre limiti arbitrari alla risoluzione per nascondere l'overflow.

## O05a — La griglia uniforme viene materializzata anche senza modifica

Lo stesso corpo `restore_face_sets_grids` chiama `faceset_grid_ensure_dense()` prima di sapere se gli ID differiscono. Nel test, master uniforme 1 e snapshot interamente 1: la funzione ritorna `false`, ma ora esiste un buffer denso. Su G griglie uniformi di area A la sola espansione richiede `4*G*A` byte persistenti, oltre al costo dello snapshot. È la dimostrazione locale dell'opportunità O05 del primo report.

**Ottimizzazione proposta:** snapshot con tag uniforme/denso e valore uniforme; confrontare prima di allocare; mantenere o ripristinare la rappresentazione uniforme quando possibile. Registrare a parte se una griglia era presente, come per Polypaint. Misurare anche quante griglie diventano dense dopo una sequenza Undo/Redo priva di cambiamenti netti.

## K03 — Join e sincronizzazione livelli: contratto incompleto per i master

Nel [percorso Join](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/mesh/mesh_join.cc#L706-L778) vengono copiati `CD_MDISPS` e mask nella mesh risultante, poi gli oggetti sorgente sono eliminati. I nostri layer, il master Polypaint e il master Face Set HR risiedono nel modifier. In quel percorso non è presente una loro migrazione esplicita. Il codice aggrega i Face Set della base, che non equivale ad aggregare i master HR.

Inoltre [`multiresModifier_sync_levels_ex`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires.cc#L2994-L3009), usato nella preparazione Join, nel ramo di riduzione chiama `multires_del_higher()` direttamente. Non passa dal nuovo wrapper che tronca i layer e valida i master.

**Stato:** rischio di perdita o disallineamento individuato nella catena; nessuna perdita di dati riprodotta con Join nativo. Prima di dichiarare il comportamento finale vanno controllati anche il trasferimento della mesh e i callback di remap.

**Intervento proposto:** decidere esplicitamente il contratto Join: rifiutare combinazioni non supportate con un messaggio preciso, oppure migrare i payload per identità/range e trasformarli nello spazio del destinatario. Usare la stessa transazione di cambio livello per tutti i chiamanti. Testare due oggetti con layer/master diversi, livelli diversi e scale diverse, mantenendo copie e verificando Undo.

## Priorità per prestazioni e architettura

L'approfondimento suggerisce di fissare questi contratti prima di aggiungere altri percorsi veloci:

| Contratto | Conseguenza pratica | Lavoro proposto |
|---|---|---|
| Payload nullo = zero valido | Ogni operazione deve conoscere la semantica sparse | Helper condivisi per lettura, validazione e materializzazione selettiva; consolidare B01/B04 |
| Payload nel modifier, base nella Mesh | Le operazioni native sulla Mesh non coprono automaticamente i layer | Matrice Apply Scale, Join, duplicate/link, cambio topologia, Apply Modifier e save/reopen |
| Master HR indipendente dal livello visualizzato | Undo a basso livello può comunque copiare il master alto | Snapshot per tile/uniformità senza ridurre né ricampionare il master |
| Cache valida rispetto a tutti gli input del limite | Un puntatore o la sola cage non bastano sempre | Revisione evaluatore e invalidazioni verificabili, poi ottimizzare HIT/BUILD |
| Memoria Undo visibile e limite devono essere coerenti | Le ottimizzazioni possono spostare il costo fuori dal conteggio | Conteggio unico e misure RSS/payload in parallelo, inclusa fase di compressione |

Ulteriori punti concreti da misurare:

- Il restore Face Set marca tutti i nodi quando cambia almeno una griglia HR (`changed_nodes = node_mask` nel ramo `modified_face_set_grids`). Raccogliere le griglie effettivamente cambiate e invalidare solo i nodi interessati.
- `grid_ensure()` invalida le cache quando materializza un payload; nel publish parallelo di molte nuove griglie questo entra ripetutamente nei mutex globali. Valutare invalidazione una volta per transazione prima dei worker, mantenendo corretta la revisione del supporto sparse.
- Mantenere separati tempo di stroke, solve, publish, compressione in background e attesa al primo Undo: un miglioramento del primo dato può spostare lavoro sugli altri.

Non sono state misurate percentuali di accelerazione Blender. Queste proposte conservano il requisito di master HR completo e il dettaglio ai livelli inferiori.

## Prove e ripetibilità

- [Script approfondimento](verify_deep_findings.py), dipendente dallo [script base](verify_findings.py) nella stessa cartella.
- [Risultati](deep-verification-results.txt) e [hash delle funzioni/espressione](deep-verification-manifest.json).
- Diagnostiche attese: [Bake sparse](deep-bake_sparse-sanitizer.txt), [buffer Face Set corto](deep-faceset_short-sanitizer.txt), [overflow soglia](deep-threshold-sanitizer.txt).
- [Blob remoti aggiuntivi verificati](deep-remote-source-manifest.json); i tre input del primo harness sono nel [manifest originale](remote-source-manifest.json).

Otto casi in processi separati: cinque osservazioni/controlli completati e tre failure attese dei sanitizer. Nei casi B04/H03 viene usato il corpo reale della funzione, in H04 l'espressione originale. B06 usa encode/getter originali con completamento del task simulato, non una riproduzione multithread di Blender. Compilazione C++17, warnings-as-errors, ASan/UBSan; LeakSanitizer disabilitato per la restrizione `/proc` dell'executor.

```sh
python /percorso/delle/note/verify_deep_findings.py /percorso/checkout-audited
```

I sanitizer sono **attesi** nei tre casi indicati: il driver verifica il fallimento dei figli e termina correttamente quando il difetto è riprodotto. I test non vanno interpretati come una certificazione di codice corretto. Nessuna build Blender completa o prova GUI è stata eseguita.

**Ordine consigliato:** unificare e rendere sicure le operazioni sparse B01/B04; sistemare le fusioni B02; correggere insieme i due conteggi Undo B03/B06; verificare Apply Scale e Join prima di considerarli compatibili con lo stack; poi procedere con snapshot uniformi e ottimizzazioni locali.
