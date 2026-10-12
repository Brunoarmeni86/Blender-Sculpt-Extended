# Verifiche eseguite e limiti

## Eseguito qui

* Recuperati direttamente i file reali GitHub dalla revisione V4
  `0abe66b8ab9b866d28da35412b56d576690e89ed`, inclusa la definizione
  `SubdivCCGCoord` per verificare l'API dell'audit cuciture.
* Patch incrementale verificata con `git apply --check` sullo snapshot base,
  controllo inverso sui sorgenti modificati e `git diff --check`.
* Compilazione GCC C++17 del kernel originale e modificato estratto da
  `sculpt.cc`, con piccoli adattatori che simulano le API Blender.
* Confronto esatto delle distanze e di tutti i componenti dei frame tra
  originale e modificato: identici con diagnostica 0, 1 e 2.
* Dieci scenari della singola verifica differenziale: patch piana con
  cucitura tra due griglie, riduzione raggio/riuso, aumento raggio,
  passata simmetrica V3, superficie curva, inversione normale,
  normali mancanti, asse invalido, superamento cap V4, CCG mancante.
* Smoke check dei contatori atomici: quattro thread, 80.000 incrementi,
  80.000 osservati. Controllo dei formati printf con `-Wformat=2`.

Questa verifica usa il codice reale di preparazione e trasporto, ma
geometrie sintetiche e API simulate. Non esegue il brush completo, il PBVH
reale, il depsgraph, Sculpt Layers o Undo. Non dimostra che il campionatore
di ogni brush fornisca sempre l'indice CCG o che la proiezione visiva sia migliore.

## NON eseguito qui

* Compilazione completa di Blender o Visual Studio 2022.
* Esecuzione della patch PowerShell: PowerShell non e disponibile qui.
* Prove su mesh Multires da decine di milioni di vertici.
* Benchmark o confronto visivo V3/V4/V5 sul modello dell'utente.
* Implementazione della proiezione V5: in questo pacchetto c'e il progetto.

Il log opzionale altera i costi di esecuzione: gli incrementi atomici
possono introdurre contesa. `search_ms` misura la costruzione prima del
campionamento e include il costo dei contatori di ricerca; il livello 2
aggiunge un audit separato. Confrontare tempi finali di sculpt con
diagnostica disabilitata. Nessuna allocazione aggiunta e proporzionale
all'intera mesh: un oggetto di diagnostica per passata, piu gli iteratori
locali dell'audit opzionale sulla cache gia esistente.
