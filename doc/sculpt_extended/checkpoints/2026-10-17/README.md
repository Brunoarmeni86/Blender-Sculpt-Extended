# Checkpoint Sculpt Extended — 17 ottobre 2026

Consolidamento incrementale da `01194d089eb5b324b125d0ef6ebaf877dd13f326`
(`checkpoint/sculpt-extended-20261015`). Le ottimizzazioni preesistenti restano
nel parent. Nessun reset, sostituzione di main o modifica alle opzioni CMake/GPU.

## Codice incluso

- Detail Transfer V01: import nativo in un nuovo layer, preview congelata e pannello Python.
  Chiamate Python con `EXEC_DEFAULT, True` per il checkpoint Undo nativo.
- Face Sets HR V01–V05: master categorico indipendente dalla geometria; Draw,
  visibilita', maschere, gesti, aggiornamento overlay e picker del pannello;
  Draw/Mask indipendenti da REC per le modalita' non geometriche supportate.
- Estrazione HR: Mask/Face Set verso nuovo oggetto, saldatura topologica e limiti
  output. Solidify usa `offset` per spessore e `offset_fac` per direzione.
- HR Mask: preparazione dello storage prima di acquisire riferimenti a CCG/PBVH;
  nuova valutazione e validazione prima di iniziare Undo. Corregge il crash nativo
  segnalato dopo la creazione della maschera.
- B10/B11: binding persistente dei corner, copia/serializzazione e remap conservativo;
  guardie Edit comprendono i master colore inattivi.
- B01/B02/B04/B08/B09/H02: fusioni sparse con preflight numerico, selezione per
  identita', fusione gruppo unica, Bake condiviso e gestione contiguita'/visibilita'.
- B05: Apply Scale uniforme positivo dei delta HR/L0, inclusi nascosti e weight zero.
  Validazione dell'intera selezione prima delle scritture; include completo MDisps.
- K03: protezione Join prima di sincronizzazione/cancellazione degli oggetti con
  companion persistenti. Non implementa la migrazione dei master.
- H03: controllo livello/dimensione nel writer denso Face Set.
- H04: moltiplicazione int64 della soglia dense-sample.

Le patch Face Set V01 includevano gia' il preflight master prima del resample,
il controllo Undo dei descrittori, swap uniforme e conteggio del buffer Face Set
HR. Il registro audit del vecchio checkpoint non descrive automaticamente lo
stato di questo ramo. Non vengono chiusi indiscriminatamente gli ID di audit.

## Evidenza nativa fornita dall'utente Windows

I risultati numerici e gli hash dei quattro log sono in [native-results.json](native-results.json).

| Fixture | Risultato osservato | Limite |
|---|---|---|
| Topology binding | PASS: Edit preserva dettaglio, ID non canonici, subdivision, Undo/Redo, duplicate, reopen | Piccola fixture geometrica; non certifica companion paint-only/legacy |
| HR extraction | PASS: Mask/Face Set Extract, 25 vertici/16 quad, sorgente invariata, Undo/Redo, limite, reopen | Nessuna simulazione reale di tutti i pennelli/gesti |
| Merge/groups | PASS: errori geometria 0.0, Flatten Undo/Redo, merge/group/remove, visibilita', Bake sparse, reopen | Piccole fixture; nessun benchmark denso |
| Safety | PASS: Apply Scale errore 0.0, layer nascosto errore 0.0, Undo/Redo, rifiuto selezione non uniforme, Join protetto/ordinario, reopen | HR nativo; L0 verificato solo con funzione estratta |

Detail Transfer aveva inoltre un precedente PASS nativo: errore preview
1.862645149230957e-09, source/reference invariati, Undo/Redo, stale preview rifiutata,
save/reopen. Il relativo log non e' incluso nei quattro file della presente raccolta.

Queste prove riguardano l'eseguibile Windows costruito dall'utente con i kit e
le correzioni inline documentate. Questo commit assembla quelle modifiche;
non e' una nuova build nativa di questo SHA e non confronta byte per byte tutti
i sorgenti della macchina Windows. Qui eseguiti assemblaggio letterale dei
blocchi, controlli di coesistenza, sintassi Python e diff. Nessun Blender/MSVC
locale. Per confermare l'identita' dei sorgenti usare [source-sha256.json](source-sha256.json).

## Strumenti ripetibili

[Avvio e fixture native](../../../../experiments/sculpt-consolidated-20261017/README.md).
Il runtime e' relativo al repository e non richiede i vecchi ZIP esterni.
I cinque test preparano scene separate; non vanno lanciati in parallelo.
Le modifiche all'Undo delle chiamate Python e alla selezione della finestra dopo
reopen includono le correzioni applicate durante i precedenti test.

## Limiti e lavoro aperto

- B06/B07: fix integrati e verificati con fixture native mirate; dettagli e limiti in
  [B06-B07-VERIFICATION.md](B06-B07-VERIFICATION.md).
- K01/K02: revisione concorrenza Undo e invalidazione completa della cache Subdiv.
- B11: manca prova nativa del solo master Polypaint inattivo durante Edit.
- Apply Scale con layer rifiuta rotazione, scala non uniforme/riflessioni,
  mesh condivise, Shape Keys, storage esterno, Multires multipli e payload invalidi.
- Join e' protetto, non trasferisce i master. Non e' implementato lo spostamento
  del dettaglio lungo la superficie o il trasferimento completo degli attributi.
- Estrazione non include Mask Slice, UV/materiali/rig; hidden pending richiede
  Commit prima dell'estrazione. Limiti output non equivalgono a limite RAM totale.
- Le segnalazioni RNA di default stringa vuota all'avvio restano da esaminare.
- Non e' dimostrato che i messaggi Wintab/allocazioni alla chiusura degli altri
  test derivino dai cambiamenti di questo ramo; nessuna dichiarazione leak-free.

Audit condiviso: [issue #3](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/3).
