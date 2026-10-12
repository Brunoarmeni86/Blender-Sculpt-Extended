# Anchor V5.3 — opzioni e diagnostica

Richiede V5.2 gia applicata nei sorgenti. La matematica V5.2 resta identica: questa
patch modifica solo RNA e interfaccia, senza nuovi campi DNA o conversioni dei file.
Non cambia la build stabile e non modifica Sculpt Layers, Undo, Commit o Edit Mode.
Ramo separato: `feature/anchored-surface-options-v53`.

## Applicazione Windows

Estrai in `C:\BlenderDev\Anchor-V53`, entra nella cartella che contiene il file
PowerShell e apri Developer PowerShell di Visual Studio 2022. Chiudi la build di test.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\APPLICA-E-COMPILA-ANCHOR-V53.ps1 -Run
```

Sorgenti predefiniti: `C:\BlenderDev\blender`.
Build di test: `C:\BlenderDev\build_windows_x64_conform_test`.
Backup automatico dei due file modificati prima della patch. Dopo la compilazione
aggiorna anche lo script UI nella build di test, se trova una sola cartella runtime,
con backup separato; altrimenti segnala che la UI va sincronizzata. Accetta anche sorgenti
estratti senza `.git`; in presenza di Git crea un ramo locale separato.
Non sostituisce file interi: se il contesto e incompatibile si ferma.
`-ApplyOnly` applica senza compilare. Non scarica dipendenze ne riconfigura CMake.
La build `C:\BlenderDev\build_windows_x64_shareable` e bloccata come destinazione.

## Selettore

In Sculpt, impostazioni Stroke, metodo Anchored, Texture Mapping Area Plane:

- **Standard** usa il percorso originale.
- **Surface Projection** abilita insieme normal guidance, surface paths e tangent mapping.

Standard resta il valore iniziale per pennelli nuovi senza flag Anchor. I pennelli
gia salvati mantengono la scelta precedente: nessuna migrazione forzata.
Le tre opzioni inferiori restano accessibili per confrontare le versioni precedenti.
Il selettore indica l'attivazione generale; se disabiliti una sotto-opzione puoi
usare gli stadi precedenti, pur mantenendo la scelta Surface Projection.
Selezionando nuovamente Surface Projection vengono abilitati tutti gli stadi.
Standard spegne il flag principale e conserva i sotto-flag per compatibilita.
I percorsi CCG richiedono Multires valutato; un modificatore visibile nell'interfaccia
non garantisce da solo l'idoneita della cache. La diagnostica mostra il percorso effettivo.

## Diagnostica senza ricompilare

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\AVVIA-DIAGNOSTICA-ANCHOR.ps1
```

Funziona anche sulla V5.2 gia compilata. Registra stdout e stderr separati. Dopo la
chiusura di Blender prova a usare il Python incluso in Blender per creare
`anchor-*.summary.json`; se non lo trova, conserva il log per l'analisi manuale.
Per un audit delle cuciture usa `-Level 2`, solo per una prova breve.
Entrambi i livelli aggiungono overhead: non usarli per misurare la velocita normale.

Il riepilogo mostra percentuali di chiamate texture per V1/V2/V3/V4/V5, richieste
rifiutate, preparazione/cache/fallback e passate specchio/radiali/tiling osservate.
`v5` comprende la proiezione V5.2. Sono chiamate texture, non vertici unici o impronte.
La preparazione comprende ricerca, rilassamento e audit: non sommare questi tempi.
Non misura il tempo completo della pennellata o la latenza dell'interfaccia.
Nessuna passata osservata significa nessuna evidenza, non necessariamente un errore.

## Prova minima sul PC

Usa la stessa mesh Multires e un'alfa asimmetrica con un dettaglio riconoscibile.
Fai una prova breve per ciascuna situazione, partendo lontano dal piano di simmetria:

1. Standard e Surface Projection nello stesso punto; verifica che lo switch funzioni.
2. Surface con specchio X: verifica orientamento specchiato e presenza di passate non principali nel log.
3. Radiale 3 su un solo asse: controlla orientamento e le tre impronte.
4. Impronta che attraversa una cucitura CCG; audit livello 2 e osservazione visiva.
5. Allarga e restringi Anchored piu volte: controlla fluidita, continuita e riuso cache.

Non confondere l'assenza del trasporto con l'assenza di pennellata: puo intervenire
un fallback. Un log valido permette di distinguere le due situazioni.
Per il confronto prestazionale ripeti senza diagnostica, con raggio e livello identici.
Queste prove reali di simmetria/cuciture restano da eseguire sul tuo PC.

## Stroke Engine

Il ramo V5.2 contiene gia lo Stroke Engine Adaptive V1/V2: interpolazione pressione,
accumulo uniforme, spacing adattivo e stabilizzazione. Questa patch non lo riscrive.
Il percorso e limitato a Draw, Draw Sharp, Clay e Clay Strips, metodo Space e distanza
schermo. Grab, Pose, Snake Hook e Anchored mantengono il motore originale.
Per una prima valutazione scegli Draw > Space > Stroke Engine Adaptive, pressione
e accumulo abilitati, spacing adattivo e stabilizzazione disabilitati. Confronta
tratti lenti e veloci a parita di pressione. La verifica sul PC non e ancora documentata qui.

## Verifica eseguita qui

Patch applicabile alla base GitHub V5.2, reversibile; sintassi Python; callback RNA
estratte compilate in un programma C++ minimo con i flag reali e verifica dei bit
estranei; parser verificato su log UTF-8/UTF-16, inclusi record malformati.
La parte di proiezione e identica alla V5.2 da te provata.
Non compilato Blender completo con MSVC. Non eseguiti PowerShell, UI Blender,
asset/undo RNA o simmetria reale in questo ambiente. Non promette un aumento di velocita.
