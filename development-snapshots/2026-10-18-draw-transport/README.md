# Sculpt Extended — freeze Draw Transport, 18 ottobre 2026

**Stato congelato per richiesta dell utente. Nessuna ulteriore ottimizzazione applicata dopo V07.**

Questo branch archivia i kit Windows e il relativo stato di avanzamento. I file sotto `source/` alla radice sono quelli del commit `main` da cui nasce il branch: **non rappresentano automaticamente la build Windows del 18 ottobre**. I sorgenti esatti consegnati per Draw Transport sono nei payload dei kit e negli archivi. Non compilare la radice del branch aspettandoti la stessa build locale. Il trasferimento in un branch di integrazione richiedera il confronto con i sorgenti Windows completi correnti.

## Riferimenti congelati

- **V06: ultimo riferimento esplicitamente confermato funzionante dall utente**, con miglioramento percepito rispetto a V05. La velocita diminuisce con impronte piu grandi; non esiste un benchmark A/B controllato tra versioni.
- **V07: ultima versione consegnata, compilata ed eseguita su Windows**, conservata come esperimento. I log confermano riuso cache e profiling; entrambi gli stroke dell ultimo log sono annullati (`cancel=1`). Conferma e Undo/Redo integrati della V07 non sono certificati da quel log. Non viene dichiarata piu veloce di V06.
- V05 confermata molto piu fluida rispetto alle versioni precedenti; V04 funzionante su cube sphere fino a circa 25 milioni, senza i vecchi tetti fissi di campioni.
- SHA256 originale dell eseguibile pulito/checkpoint verificato dall utente: `68D8DD4783DA2983ABB922FC8CF5689469598EE3C5614D35EC030026F144529D`. Questo hash **non identifica** gli eseguibili V06/V07 successivi. Il binario Windows non e incluso qui.

## Funzioni ottenute

- Trasferimento dettaglio fra mesh V01: PASS utente per anteprima, Undo/Redo, salvataggio e riapertura.
- Copia di una regione del layer sulla stessa mesh con estensione Python: funzionante; archivio Copy V01 incluso.
- Pennello locale dedicato Draw Transport in Sculpt Mode: cattura tramite Draw Anchored, rilascio per posizionare, traslazione sulla superficie, R rotazione, S scala tangente con profondita invariata, SS Strength relativa della sola impronta.
- Clic/Invio accetta una regolazione; durante il posizionamento conferma lo stroke nel layer REC attivo. Esc/destro annulla lo stroke.
- Il delta resta quello dello stroke corrente: nessun accumulo delle anteprime. Non e ancora un pennello per selezionare e spostare arbitrariamente dettaglio preesistente.
- Scrittura tramite `apply_translations` nativo, journal dirty grids/samples e transazione Undo nativa mantenuti.
- Nessun nuovo tetto fisso artificiale di campioni; restano memoria disponibile, validita dati e rappresentabilita indici BVH.

## Ottimizzazioni archiviate

V05: ripristino esatto locale delle coordinate; cache regione e mapping SS; ricerca PBVH gerarchica; ricampionamento e scrittura su griglie disgiunte paralleli; trasformazioni precalcolate; tempi per fase.

V06: triangoli precedenti/vicini come suggerimenti per restringere il limite della ricerca BVH, mantenendo la ricerca globale e il fallback.

V07: riferimenti conservati tramite ID griglia quando la regione cresce alla stessa destinazione; scarto conservativo fuori AABB sorgente espansa; skip di campioni completamente mascherati; contatori per query/miss/fallback/cull/hint. Resta sperimentale.

## Evidenze di prestazione (impronte diverse, nessun rapporto di accelerazione deducibile)

| Versione/log | Evidenza |
| --- | --- |
| V05 09:54 | MOVE 89–111 ms nei frame mostrati; SS 10.5 ms; R 795 ms con 775 ms map, 2.15 milioni di campioni |
| V06 10:21 | MOVE 78–130 ms; R 54–87 ms; SS 11–38 ms; S 307 ms e un frame 1721 ms con 1660 ms map |
| V07 10:32, prima impronta | 2.565 milioni triangoli sorgente; 25 frame; media map 1045.974 ms; annullata. Il campionamento del log non mostra tutti i picchi |
| V07 10:32, seconda impronta | 1.519 milioni triangoli sorgente; S conserva 1.69–2.04 milioni campioni; S 311–486 ms nei frame mostrati; media map 176.443 ms; annullata |

Nel log V07 mostrato, AABB scarta pochi campioni e i frame registrati hanno fallback zero. Il riuso cache funziona ma non dimostra da solo un guadagno. I log filtrati in `profiles/` contengono solo le righe Transport/Commit; gli errori separati riguardavano Wintab e HairBricks, non Draw Transport.

## Validazione e limiti

Test C++ standalone con funzioni effettive del payload e adattatori sintetici delle API Blender: 100000 confronti trasformazioni, 86400 confronti kernel, 36000 confronti posizioni su 90 frame MOVE/R/S/SS/fuori mesh/crescita/riduzione; errore nullo nei fixture, output seriale/parallelo identico. V07 verifica inoltre riuso su 3775 campioni cumulativi, scarto AABB, seed invalidi e risultati equivalenti con cache/cull disabilitati. Ripristino Undo verificato tramite adattatore, non sostituisce il test dentro Blender. I test C++ sono nei kit.

Nessuna build Blender completa e stata eseguita nell ambiente Codex. Le compilazioni e prove Windows sono quelle dell utente. Scala tangente 0.05x–8x, Strength relativa 0x–10x. Richiede Draw Anchored, Multires al massimo livello, mesh locale single-user, niente simmetria, Dyntopo o deformazioni live. Ricampionamento locale con normali, non trasporto geodetico esatto su pieghe/superfici sovrapposte.

## Isolamento dagli altri lavori

Face Sets V07 e successiva patch di visualizzazione erano stati rimossi dalla build Windows funzionante. Face Sets base funzionanti; propagazione HR incompleta e sospesa. **Non riapplicare Face Sets HR per usare questi kit.** Cloth ha ancora registrazione layer incompleta. Non ripartire dai test generali dei pennelli.

La modalita Sposta con cancellazione atomica della sorgente del dettaglio preesistente resta da sviluppare; e distinta dal posizionamento del nuovo stroke Draw Transport.

## Ripresa futura (idee non implementate)

1. Registrare tutti i frame lenti e distinguere costo delle query riuscite e senza risultato.
2. Confrontare attraversamento BVH standard e `BVH_NEAREST_OPTIMAL_ORDER`; la coda per query puo annullare il vantaggio.
3. Valutare raggio progressivo con ritorno al limite attuale, senza troncamento dei risultati.
4. Misurare precalcolo dei dati invarianti dei triangoli contro costo di memoria/cache CPU.
5. Embree closest-point solo come eventuale esperimento separato. Nessuna promessa GPU o anteprima ridotta introdotta.

## Contenuto e riproduzione

- `kits/`: V06 e V07 leggibili, con payload C++, loader/UI, applicazione/compilazione, launcher, rollback, manifest hash e test.
- `archives/`: ZIP esatti consegnati V01–V07, Copy V01 e l export sorgenti originale ricevuto (subset; non repository completo). Integrita in `ARCHIVE-SHA256.json`.
- `CREA-CHECKPOINT-SCULPT-20261018.ps1`: script checkpoint originale.
- Applicare i kit solo alle basi riconosciute dai manifest, tramite gli script con controllo hash e backup. Non copiare alla cieca i payload sopra sorgenti successivamente modificati.
- Nessuna integrazione su main, nessuna modifica Face Sets HR in questo freeze.
