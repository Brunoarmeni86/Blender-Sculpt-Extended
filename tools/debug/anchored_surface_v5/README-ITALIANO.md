# Anchored Surface Projection V5 — prima implementazione sperimentale

Ramo GitHub separato: `feature/anchored-surface-projection-v5`.
Base: V4.1 `fbd61f468e86b84ffb49220851f65161bbd170a2`.
Incluse patch incrementali alternative da V4 originale e da V4.1.
Lo script ne sceglie una: NON applicarle entrambe.

## Avvio rapido

Estrai lo ZIP. Nella Developer PowerShell di Visual Studio 2022, entra nella
cartella estratta ed esegui:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\APPLICA-E-COMPILA-ANCHOR-V5.ps1 -Run
```

Sorgenti: `C:\BlenderDev\blender`.
Build: `C:\BlenderDev\build_windows_x64_conform_test`.
Eseguibile: `C:\BlenderDev\build_windows_x64_conform_test\bin\Release\blender.exe`.
La directory `build_windows_x64_shareable` e le sue sottocartelle sono rifiutate.
Lo script salva un backup dei due file prima di modificarli, verifica la patch,
crea un ramo locale se il sorgente ha .git, e compila la configurazione esistente.
Supporta anche sorgenti estratti senza .git, usando git apply come patcher.
Non esegue reset, pull, reconfigure, commit o sostituzione completa dei sorgenti.
I messaggi di git relativi a patch non applicabili durante l'autodetezione
sono attesi: conta il risultato finale, non il primo tentativo scartato.

## Come provarla

Usa il brush esistente con alfa, Anchored, Area Plane e i toggle gia presenti:
Surface Projection + Geodesic + Transport. La V5 sostituisce il percorso
Transport quando abilitato; non aggiunge un brush o un nuovo campo DNA.

Test minimo consigliato: alfa asimmetrica (lettera/griglia) su naso/guancia,
poi stessa impronta con simmetria X e ridimensionamento interattivo.
Confronta con V4 nella stessa build riavviando con `BLENDER_ANCHOR_PROJECTION_V5=0`.
Il confronto V4 resta senza i vecchi limiti di campioni.

Per avvio con diagnostica:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\APPLICA-E-COMPILA-ANCHOR-V5.ps1 -Run -DiagnosticLevel 1
```

Con `-DiagnosticLevel 2` viene controllata anche la continuita dei controlli
di cucitura e l'ortogonalita dei frame. `-Legacy` avvia V4 nella stessa build;
`-ApplyOnly` applica senza compilare. Le variabili di ambiente vengono ripristinate
dopo la chiusura di Blender. Per un confronto visivo usa diagnostica spenta.

## Implementato

* Rimossi i limiti di 100.000 e 300.000 campioni, anche da V4/V3.
  Non c'e piu fallback causato dal conteggio dei campioni.
* Lattice di controllo locale, allineato alle griglie CCG; campionamento alfa
  ad alta risoluzione tramite interpolazione delle coordinate, senza abbassare
  il livello Multires o modificare la geometria.
* Densita scelta dalla spaziatura locale e dal raggio. Raffinamento dell'intera
  patch se gli edge rivelano pieghe o perdita di trasporto: non e ancora un
  quadtree con raffinamento differente per ciascuna cella.
* Lunghezze integrate sui campioni high-res lungo gli edge del lattice;
  non si misura semplicemente la corda attraverso una concavita.
* Identita topologica condivisa per i duplicati delle cuciture CCG.
* Trasporto iniziale + cinque iterazioni massime di correzione metrica locale,
  accettate solo se migliorano l'energia. Protezione contro nuove inversioni
  dei triangoli UV rispetto all'inizializzazione; non e una garanzia globale
  contro overlap o tagli geodetici gia presenti.
* Trasporto V5 su simmetria speculare, radiale e tiling, con gestione del verso
  degli assi. Cache indipendente per ciascuna passata.
* Rotazione dell'alfa e riduzione del raggio riusano la cache. Aumentare il raggio
  oltre la copertura ricostruisce la patch.
* Restano V4/V3/V2/V1 come fallback per frame invalidi o percorsi non supportati.
  Se V5 fallisce in una passata speculare/radiale, il fallback sicuro e V3.
* Con eccezioni C++ abilitate (la configurazione Windows usa /EHsc), una reale
  std::bad_alloc libera le cache e usa V1/V2 per quella preview. Questo non
  introduce un nuovo tetto artificiale. Gli allocator che abortiscono o una
  terminazione del processo da parte del sistema operativo non sono intercettabili.

La memoria e proporzionale alle patch toccate, sommate tra le passate, e non
preallocata per tutta la mesh. Un'impronta enorme o pieghe che obbligano stride=1
possono comunque costare molto: rimuovere il cap non elimina il costo reale.
Le curvature interamente interne a una cella e non intercettate dagli edge
possono sfuggire al criterio di raffinamento di questa prima versione.

## Diagnostica aggiornata

`v5` conta le chiamate al campionatore che usano le nuove coordinate; non i
vertici unici. `visited` e il numero di controlli canonici finali, `frames`
la copertura tangente. `stride` e la distanza tra controlli in coordinate CCG:
1 significa lattice completo. `refinements` conta le riduzioni dello stride.

`search_ms` include costruzione/adattamento del dominio e lookup dei controlli,
escludendo `relax_ms` e `audit_ms`. `prepare_ms` include anche ricerca seed e
allocazioni. `metric_before/after` e la somma dei quadrati dello strain degli
edge: confrontabile prima/dopo sulla stessa patch, non tra patch di taglia diversa
e non una misura diretta della qualita visiva dell'alfa.

I contatori di ricerca includono tentativi di raffinamento, mentre i controlli
contati sono quelli finali. Nel riuso i tempi di ricerca/solver/audit sono zero;
stride e metrica descrivono la cache riutilizzata. Audit seam: i duplicati usano
gli stessi dati canonici, quindi lo zero atteso verifica questa identita; non
dimostra l'assenza di distorsione all'interno delle celle.

## Verificato e non verificato

Compilati e provati qui i kernel estratti con adattatori CCG/PBVH simulati:
cucitura ruotata, separazione di fogli vicini, speculare X, radiale 90 gradi,
cache per passata, resize/rotazione, adattamento e diminuzione dell'energia.
V4 oltre 100.000 frame e V3 oltre 300.000 campioni completano senza cap.

NON compilata la build completa Blender/Visual Studio 2022 qui.
NON eseguito lo script PowerShell qui. NON validati ancora la qualita visiva
e i tempi sulle tue Multires reali. La prova sul PC resta necessaria.
Dettagli in `VALIDAZIONE.md`.
