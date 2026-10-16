# Terzo approfondimento — animazione, gruppi e identità dopo Edit

Segue il [primo audit](README.md) e il [secondo approfondimento](DEEP-REVIEW.md). Sorgente sempre fissato a `01194d089eb5b324b125d0ef6ebaf877dd13f326`, checkpoint ricontrollato sul remoto. All'inizio di questa passata main era `10623050c033c4d84d64378ced3c5eba0bb11303`, che aggiunge documentazione e riproduzioni al precedente main.

**Nessuna correzione applicata al codice Blender.** Questa passata aggiunge 14 osservazioni/controlli su funzioni o condizioni estratte. Non è stata eseguita una build Blender, un playback della timeline o una sequenza Edit/BMesh/Save nativa. Le patch più recenti distribuite fuori dal checkpoint non sono certificate.

## Riscontri aggiuntivi

| ID | Priorità | Riscontro | Prova |
|---|---|---|---|
| B07 | P1 | Copy-back dell'animazione sullo Strength originale entra nel percorso interattivo Sculpt | Setter e dispatch animazione reali; A1–A5; risoluzione RNA e Commit sono adattatori |
| B08 | P2 | Rimuovere il membro centrale lascia un gruppo non contiguo che Merge Group rifiuta | Funzioni reali; G1–G2 |
| B09 | P2 | Unire un gruppo nascosto a uno visibile non ripristina la visibilità salvata dei membri | Funzioni reali; G3–G4 |
| B10 | P1 | Remap senza old IDs presume ordine canonico dopo che gli ID stabili sono diventati non canonici | Remapper e kernel reali, storage con payload reali; T1–T4 |
| B11 | P1 condizionato | Le guardie Edit Mode ignorano i master Polypaint soltanto inattivi | Condizione originale e controlli T5; mancata migrazione end-to-end non testata in Blender |

P1 significa dare priorità al percorso interessato prima di considerarlo compatibile con lo stack. Non significa che ogni file o pennellata produca il problema.

## B10 — Dettaglio perso al secondo remap con ID stabili non canonici

Questo è il riscontro più importante della passata per la conservazione dei dati.

Il sistema usa ID persistenti dei corner, non indici di array. Dopo la cancellazione della prima faccia di due quad, è corretto che la faccia superstite conservi ID `5,6,7,8`, mentre le sue quattro griglie occupano ora gli indici `0,1,2,3`.

[`remap_after_topology_edit`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2220-L2259), quando riceve `old_corner_ids` vuoto, costruisce invece `storage_ids = 1..old_corners_num`. Su quel secondo stato associa quindi i quattro ID superstiti a quattro corner nuovi (`-1`). Il controllo di orientamento non può recuperare un'identità già persa nella lookup.

Il [remapper dei payload](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L1971-L2004) tratta giustamente `-1` come nuovo corner: crea il contributo zero e libera il vecchio payload non trasferito.

### Sequenza riprodotta con entrambi i remapper reali

1. Storage iniziale con 8 griglie e payload distinti; nuova mesh-adattatore con un quad superstite, ID `5,6,7,8` e vicinato ciclico coerente.
2. Remap con old IDs espliciti `1..8`: i payload superstiti sono conservati e lo storage si riduce a quattro griglie.
3. Nessun ulteriore cambio di topologia o orientamento.
4. Remap con old IDs omessi: **tutti e quattro i payload superstiti vengono liberati e sostituiti da zero implicito**.

Controlli: con old IDs espliciti `5,6,7,8`, nessuna perdita; con ID canonici `1,2,3,4`, anche il default conserva i payload. Il test contiene vere allocazioni e usa i corpi originali di `remap_after_topology_edit` e `remap_surviving_corners`; attributi Mesh, liste e wrapper L0/ID-property sono adattatori.

### Chiamanti controllati

- [Uscita/flush Edit Mode](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/mesh/editmesh_utils.cc#L384-L407): cattura gli old IDs e li passa esplicitamente. Questo comportamento è corretto per la corrispondenza ordine/ID.
- [Suddivisione con layer](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_reshape.cc#L751-L765): chiama il remap **senza** old IDs. Dopo un Edit valido che produce ID non canonici, questo preflight può quindi azzerare il dettaglio prima della suddivisione.
- Il [contratto nell'header](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/BKE_multires_sculpt_layers.hh#L89-L100) documenta il default come ordine canonico legacy. Il problema è usarlo quando lo storage ha già ID stabili non canonici.

**Limite:** non è stata eseguita la sequenza nella UI Blender. La perdita è stata riprodotta nei remapper reali nello stato di storage descritto. L'estensione a Polypaint/Face Set è indicata dal fatto che lo stesso remapper gestisce gli altri master, ma questo test misura direttamente i payload dei layer geometrici.

**Correzione proposta:** rendere esplicito il legame tra ordine dello storage e ID persistenti; non ricostruirlo da `i+1` salvo migrazione legacy verificata. L'ordine può essere conservato insieme allo storage, con serializzazione e Undo coerenti. Come protezione immediata, rifiutare il default su ID non canonici invece di distruggere i payload. Per un chiamante che sa di non aver cambiato topologia, usare un controllo dedicato e una revisione/binding verificabili.

Non passare indiscriminatamente gli ID correnti come “vecchi” in ogni caso: nasconderebbe i veri cambiamenti di topologia. Non rinumerare globalmente gli ID per farli tornare canonici senza considerare BMesh, Undo e il contatore high-water.

**Test nativo prioritario:** eliminare una faccia precedente a quella scolpita, uscire da Edit, controllare il dettaglio, poi Subdivide; ripetere con save/reopen tra Edit e Subdivide, riordino facce, Undo/Redo e master colore/Face Set.

## B07 — Il copy-back animato richiama il setter interattivo

La protezione di [`rna_MultiresSculptLayer_strength_set`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/makesrna/intern/rna_modifier.cc#L1462-L1528) copre l'ID valutato: cambia solo lo scalare. È confermato dal controllo A1.

Tuttavia, per il depsgraph attivo, [Blender abilita `flush_to_original`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/anim_sys.cc#L4065-L4077). Il percorso [`animsys_write_orig_anim_rna`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/anim_sys.cc#L537-L575) costruisce un puntatore all'originale, risolve il percorso e scrive il valore attraverso [`BKE_animsys_write_to_rna_path`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/anim_sys.cc#L469-L535). La scrittura Float usa il setter RNA; il [dispatch RNA](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/makesrna/intern/rna_access.cc#L3588-L3622) richiama `fprop->set`.

Sull'originale in Sculpt, `DEG_is_evaluated_id` è falso e il setter entra nel percorso interattivo: chiama Commit, spegne REC e chiede il refresh della superficie.

### Osservazioni del test

| Caso | Chiamata Commit | REC originale | Risultato |
|---|---:|---|---|
| A1: scrittura sull'ID valutato | 0 | Invariato | Controllo corretto |
| A2: copy-back di un valore diverso sull'originale Sculpt | 1 | 1 → 0 | Effetto collaterale riprodotto |
| A3: copy-back dello stesso valore | 0 | Invariato | Il sistema animazione evita la scrittura |
| A4: originale in Object Mode | 0 | Invariato | Aggiorna soltanto lo scalare |
| A5: Commit simulato come rifiutato | 1 | Resta 1 | Il valore originale resta vecchio e viene richiesto un rebuild |

**Limiti:** sono originali il setter e tre funzioni del dispatch animazione. Risoluzione del path, wrapper della proprietà Float, Commit, tagging e refresh sono adattatori; la chiamata a Commit viene contata, non viene eseguito un Commit geometrico. Nessun deadlock, race o danno al file è dichiarato riprodotto. La sequenza dimostra la sovrapposizione di responsabilità del setter e il cambiamento reale del flag REC nell'adattatore.

**Correzione proposta:** separare la scrittura animabile del valore dalle transazioni interattive. Il percorso UI/operatori deve gestire Commit e Undo, mentre la scrittura da animazione non deve modificare lo stato originale di registrazione o avviare una transazione Sculpt. Mantenere i corretti tag per la valutazione della geometria. Prima del fix verificare anche driver, NLA e sincronizzazione all'originale durante Sculpt.

## B08 — Rimozione da gruppo viola la contiguità richiesta

[`group_remove_active`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2723-L2741) azzera il gruppo sul membro ma lo lascia nella posizione corrente. Tre membri contigui A/B/C con ID 7 diventano quindi `7 / 0 / 7` se viene rimosso B dal gruppo.

[`group_merge`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2613-L2650) rifiuta quel gruppo perché non contiguo. G1 riproduce il rifiuto senza cambiare la topologia o introdurre metadata malformati. G2 rimuove invece il primo membro: i restanti sono contigui e vengono fusi correttamente nel caso controllato.

È distinto da B02: qui la fusione viene respinta prima di partire; B02 riguarda l'indice attivo dopo una fusione già avvenuta.

**Correzione proposta:** mantenere un unico invariante per tutti i comandi. Se i gruppi devono essere contigui, estrarre il membro a un bordo del gruppo e preservare la selezione per identità. Rivedere anche Move Up/Down, che attualmente non applica un vincolo di gruppo. Se si vogliono gruppi non contigui, il resto degli operatori e il pannello devono supportarli esplicitamente.

## B09 — Trasferimento tra gruppo nascosto e gruppo visibile

Quando un gruppo viene nascosto, il codice conserva la visibilità individuale in `MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE` e disattiva il flag visibile effettivo.

[`group_with_below`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2488-L2531) riusa [`group_assign`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2470-L2486), che imposta il nuovo `group_flag` ma non recupera la visibilità dal vecchio gruppo nascosto.

G3: gruppo A visibile sopra gruppo B nascosto; unire col comando With Below. B passa nel gruppo visibile ma resta individualmente nascosto con il vecchio saved flag. Il successivo hide/show del gruppo sovrascrive quel flag e perde lo stato visibile originario di B.

G4 è un controllo utile: [`group_add_active`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2588-L2610) gestisce esplicitamente l'uscita dal vecchio gruppo nascosto e ripristina la visibilità nello stesso tipo di trasferimento.

**Correzione proposta:** centralizzare l'uscita/ingresso da un gruppo, preservando la visibilità individuale prima di applicare il nuovo stato di gruppo. Non rendere indiscriminatamente visibili i membri: quelli nascosti individualmente devono restare tali. Testare tutte e quattro le combinazioni gruppo sorgente/destinazione visibile/nascosto.

## B11 — Guardie Edit Mode incomplete con solo Polypaint inattivo

Entrambi i controlli in [`EDBM` ingresso](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/mesh/editmesh_utils.cc#L319-L330) e [uscita/flush](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/editors/mesh/editmesh_utils.cc#L398-L407) usano:

```cpp
!mmd->sculpt_layers.is_empty() || mmd->sculpt_color_grids ||
mmd->sculpt_face_set_grids
```

Manca `!mmd->sculpt_color_attributes.is_empty()`. Eppure il [remapper generale](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L2167-L2200) e `ensure_corner_ids` conoscono correttamente i master inattivi.

Lo stato è previsto dal multi-attributo: dipingere A, poi [attivare B senza master HR ancora materializzato](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/blenkernel/intern/multires_sculpt_layers.cc#L783-L803) sposta il master di A nella directory inattiva e lascia nullo lo slot attivo. Se non ci sono layer geometrici o Face Set HR, i due controlli saltano la preparazione e il remap.

T5 esegue la condizione originale con un master inattivo allocato e un campione colore presente: la guardia è falsa. Aggiungere un layer la rende vera. Il test dimostra l'omissione della guardia; non esegue la conversione BMesh e non misura direttamente un file corrotto o una perdita di colore.

**Correzione proposta:** usare un unico helper `has_persistent_companions` che includa tutti i proprietari di payload, e condividerlo tra ingresso, uscita, flush durante Edit, remap e operazioni sulla topologia. Testare Edit Mode con layer assenti, master attivo vuoto e almeno un master inattivo dipinto; provare anche un cambio di topologia a numero di corner invariato.

## Ipotesi controllate senza aggiungere falsi positivi

- **Shape Key e cache del supporto:** la chiave locale usa diversi puntatori e conteggi, ma il percorso RNA delle coordinate ha un [update callback](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/makesrna/intern/rna_key.cc#L921-L930) che chiama [`rna_Key_update_data`](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/makesrna/intern/rna_key.cc#L728-L742), dove viene invalidata la cache. La sola presenza di una chiave basata su puntatori non basta quindi a dichiarare un bug nelle normali scritture RNA. Non vengono certificati tutti gli altri writer nativi.
- **Rename layer animato:** il [setter del nome](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/makesrna/intern/rna_modifier.cc#L1418-L1450) rende il nome unico e aggiorna i path di animazione. Non viene contato come difetto un generico “rename rompe le keyframe” non dimostrato.
- **Serializzazione dei master:** i [writer](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/modifiers/intern/MOD_multires.cc#L176-L224) e i [reader](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/01194d089eb5b324b125d0ef6ebaf877dd13f326/source/blender/modifiers/intern/MOD_multires.cc#L263-L316) comprendono slot colore attivo, directory inattiva e Face Set HR. Questa lettura non è un round-trip .blend: non autorizza né a dichiarare la persistenza perfetta né a sostenere che i master inattivi non vengano salvati. Il problema B11 riguarda i chiamanti Edit, non l'assenza di quei blocchi dal writer.

## Ottimizzazioni e contratti che emergono dalla passata

| Intervento | Motivo concreto | Prova richiesta prima di abilitarlo |
|---|---|---|
| Setter animabile puro, transazioni UI esplicite | Evitare Commit/refresh nascosti nel copy-back per ogni variazione animata | Timeline, driver, NLA, Sculpt con REC, pending stroke e Object Mode |
| Binding ordinato ID → storage con revisione | Evitare lookup o remap distruttivi quando la topologia è già sincronizzata | BMesh, save durante Edit, Undo, riordino facce e Subdivide |
| Guardie comuni per tutti i master | Non saltare dati inattivi per accelerare un controllo | Master attivi/inattivi, layer assenti, Face Set assenti, topologia invariata/cambiata |
| Gestione gruppo condivisa | Evitare ricostruzioni per rimediare a stati incoerenti e duplicazione di logica | Tutte le transizioni di visibilità e tutti i movimenti nella lista |

Non sono misure di accelerazione del viewport e non promettono una percentuale di guadagno. Prima va resa corretta l'identità dei dati; poi si può usare quella revisione per evitare scansioni e rebuild inutili.

## Riproducibilità

- [Script animazione/gruppi](verify_animation_groups.py): 9 osservazioni/controlli A1–A5, G1–G4; [risultati](animation-groups-results.txt), [manifest](animation-groups-manifest.json).
- [Script persistenza corner](verify_corner_persistence.py): 5 casi T1–T5; [risultati](corner-persistence-results.txt), [manifest](corner-persistence-manifest.json).
- [Git blob SHA aggiuntivi](animation-topology-source-manifest.json). Gli input già verificati restano elencati nei manifest dei report precedenti.

Entrambi gli script importano `extract` e/o gli adattatori da [verify_findings.py](verify_findings.py), che deve essere nella stessa cartella. Usano C++17 con warnings-as-errors e ASan/UBSan; nessuna diagnostica nei 14 casi. LeakSanitizer è disabilitato per la restrizione `/proc` dell'executor.

```sh
python /percorso/delle/note/verify_animation_groups.py /percorso/checkout-audited
python /percorso/delle/note/verify_corner_persistence.py /percorso/checkout-audited
```

Le asserzioni documentano il comportamento difettoso del commit esaminato e i relativi controlli. Non sono test di accettazione di una build sana: una correzione dovrà aggiornare le aspettative e aggiungere la verifica nativa dei chiamanti.

**Priorità aggiornata:** B10 va affrontato prima di considerare sicura la sequenza Edit → Subdivide con layer. Poi B07 e B11, insieme alla chiusura dei difetti di perdita/fusione già documentati. B08/B09 richiedono una correzione coerente delle operazioni sui gruppi. Conservare gli ID dei riscontri nelle patch e registrare commit e verifiche prima di segnarli risolti.
