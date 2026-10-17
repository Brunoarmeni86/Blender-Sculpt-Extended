# Verifica B06/B07 — 17 ottobre 2026

Questo commit incrementale mantiene il parent consolidato e aggiunge i fix
B06 e B07 verificati con gli ZIP Windows, senza modificare il branch main.

## B06 — Memoria Undo asincrona

La dimensione di uno step Sculpt viene letta senza aspettare la compressione:
una prenotazione conservativa prima del worker, poi byte compressi pubblicati
atomicamente. UI e limite cronologia usano lo stesso getter. Le ricompressioni
Undo/Redo aggiornano il conteggio per differenza. Non è un limite rigido sulla
RAM totale; quando si elimina uno step ancora in compressione resta la normale
attesa necessaria per la sua distruzione.

Log Windows `verifica-20261017-054502-093.log`, SHA256
`8cd0dc564c9c8da725150edbd0c076b220d9b3f8ca9ad78684431b430b1e7d3e`.
PASS: 3 filtri Mesh con errore Undo/Redo 0.0, Mask Undo/Redo, filtri
Multires Undo/Redo; osservato passaggio reale pending 1→0. Fixture
[Undo](../../../../experiments/sculpt-consolidated-20261017/tests/undo-memory/TEST-IN-BLENDER.py).
La potatura con budget basso e le scritture atomiche concorrenti sono testate
in C++ locale con corpi reali adattati, non forzate nel processo Blender.
Non sono state provate pennellate native Cloth/Pose/persistent layer.

## B07 — Strength animata

La scrittura sul layer valutato è scalare. Il copy-back verso l'ID originale,
sia per Action layered che per F-Curve/NLA legacy, usa ora lo stesso marcatore
locale al thread: non chiama Commit, non spegne REC e non esegue refresh locale
durante la scrittura. Lo slider manuale resta nel percorso interattivo
preesistente. Nessuna scansione aggiuntiva della mesh.

Il primo ZIP B07 copriva solo il percorso legacy e ha FALLITO il test Action
(REC spento al primo cambio di frame); il fix Action successivo è incluso qui.
Log Windows `verifica-20261017-060423-123.log`, SHA256
`6148feae5fc1506ebbd79df77800cbf994ed6ef4c7ba595296120cc634d573fc`.
PASS: 4 frame di Action con REC persistente e modifica manuale Strength che
termina REC. Fixture [Action](../../../../experiments/sculpt-consolidated-20261017/tests/strength-animation/TEST-IN-BLENDER.py).
Non testati nativamente driver, NLA, pennellata Sculpt ancora in corso né
equivalenza geometrica su layer con dettaglio non nullo.

Nessuna compilazione completa è stata eseguita per il commit ricomposto su
GitHub: le verifiche native sono state eseguite dagli ZIP sul sorgente Windows.
Le fixture GUI richiedono una build Blender con tutte le modifiche integrate.
Restano le segnalazioni RNA di default stringa vuota all'avvio, indipendenti
dagli esiti delle due fixture.
