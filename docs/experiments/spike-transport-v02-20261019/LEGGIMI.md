# Spike Multires — revisione V02 per la build in sviluppo

Questa revisione sostituisce l'installer V01. Integra l'esperimento direttamente
nei sorgenti correnti: non copia un vecchio `multires_reshape_smooth.cc` e non
richiede il ritorno a un checkpoint.

## Compatibilita esaminata

Ultimo pacchetto recuperato: FaceSets-Rebuild-Dense-Regions-V06d, base V06c +
Sculpt No-REC V01. Il relativo PATCH-STATE e il LEGGIMI confermano che la patch
FaceSets non modifica il reshape geometrico. La V02 supporta sia la cache
compatta `detail_delta_tangent` sia la precedente cache `P + tangent_matrix`.

Applicazione verificata sulle seguenti copie sorgente:
- `multires_reshape_smooth(1).cc` salvato il 15 ottobre, cache compatta.
- `checkpoint/sculpt-consolidated-20261017`, cache compatta.
- `checkpoint/draw-transport-freeze-20261018`, cache completa.

Il contenuto esatto di `multires_reshape_smooth.cc` sul PC Windows non era
accessibile: il controllo finale degli ancoraggi avviene sul PC prima di scrivere.
Un sorgente diverso o ambiguo viene rifiutato. Non viene dichiarata una build
Blender riuscita: qui non erano disponibili eseguibile e dipendenze.

## Integrazione nella cache compatta

Il residuo viene proiettato nella base ortonormale originale PRIMA di aggiornare
l'evaluator e ricostruito nella nuova base dopo l'aggiornamento. La cache conserva
esattamente tre float per campione, come prima: non ricrea la vecchia cache di
posizione + matrice. Nessun buffer aggiuntivo proporzionale ai vertici; il modo
viene letto una volta per contesto. Il costo CPU resta da misurare in Blender.

La modalita `rotation` conserva l'ampiezza del dettaglio attraverso una rotazione,
impedendo l'amplificazione dovuta allo skew delle basi. Non applica smoothing e
non limita il dettaglio in base alla sua ampiezza. La modalita `native`, ancora
predefinita, conserva il calcolo precedente.

Politica esplicita per basi degeneri: usa gli assi oggetto come riferimento
sostitutivo a ciascuno dei due stadi. Conserva l'ampiezza di un residuo finito,
ma l'orientamento vicino al collasso deve essere verificato visivamente. Questa
politica differisce dal fallback V01 e rende identici i due percorsi cache.
La patch non corregge residui gia corrotti o non finiti.

## Preservazione della build

Sono modificati soltanto il punto di trasporto in `multires_reshape_smooth.cc`
e il nuovo header locale `multires_stable_detail_transport.hh`.
L'installer registra e confronta gli hash degli altri file presenti elencati in
CURRENT-BUILD-PATHS.json, derivato dallo stato V06d: FaceSets, No-REC, layer,
Undo, Draw Transport, Delta Smooth e reshape. Non sostituisce questi file.
Il controllo CMake verifica che si compili proprio il SourceRoot richiesto.

Nei test di installazione le cinque payload V06d rimangono identiche byte per
byte per tutte le varianti e per file LF/CRLF. Questo verifica la conservazione
dei sorgenti, non l'assenza di effetti indiretti a runtime: il reshape e condiviso
anche da operazioni diverse dalla scultura e vanno provati Bake/Conform/Undo.

## Installazione e compilazione

Chiudere Blender. Estrarre il pacchetto in una cartella e aprire la Developer
PowerShell VS2022 usata per la build corrente:

```powershell
powershell -ExecutionPolicy Bypass -File .\INSTALLA-E-COMPILA.ps1
```

Percorsi predefiniti:
- SourceRoot: C:\BlenderDev\blender
- BuildRoot: C:\BlenderDev\build_windows_x64_shareable

Sono configurabili con -SourceRoot e -BuildRoot. Il compilatore usa due job;
-Jobs permette di cambiarli. CMake viene cercato anche nel cache della build.
E richiesto `python` nel PATH. Non usare insieme ad altre compilazioni attive.
Se la build fallisce, il backup resta e si puo riprovare con COMPILA.ps1 senza
riapplicare la patch. Il log e salvato nella cartella del pacchetto.

Se V01 e gia installata, ripristinare il SOLO sorgente spike con il suo backup
prima di applicare V02. L'installer riconosce entrambe le versioni gia applicate
e rifiuta una seconda applicazione.

## Prova A/B nella stessa build

Ripartire sempre dalla stessa copia del blend salvata PRIMA degli spike:

```powershell
.\AVVIA-TEST.ps1 -Mode native -BlendFile 'C:\percorso\test.blend'
.\AVVIA-TEST.ps1 -Mode rotation -BlendFile 'C:\percorso\test.blend'
```

Chiudere Blender fra i due avvii. Provare separatamente: cambio di livello senza
pennellate; piccola modifica a livello basso e ritorno in alto; nuova suddivisione.
Ripetere senza layer, con uno e con due layer, anche con strength diverso da 1.
Controllare cuciture, poli, pieghe, picchi intenzionali, Undo/Redo e Bake/Conform.
Il trasporto a rotazione differisce dal metodo affine sotto shear: e ancora
un esperimento, non una soluzione confermata sulla scena dell'utente.

## Verifica effettuata

Helper C++ compilato con GCC C++17, -Wall -Wextra -Werror -pedantic.
Identita, rotazione, andata/ritorno, collasso, basi non finite, skew e picco
intenzionale: PASS. 100.000 coppie casuali: ampiezza e round trip PASS.
Caso sintetico: guadagno nativo 932,599, stabile 1. Il calcolo e svolto tramite
encode/decode, gli stessi due stadi usati dalla cache compatta.

Installer: tre sorgenti reali, ciascuno LF e CRLF; dry-run, applicazione,
payload V06d invariati, rifiuto duplicato, protezione da ripristino dopo altre
modifiche, ripristino esatto: PASS. Vedere INSTALLER-RESULTS.txt.
PowerShell, build Blender completa e scena dell'utente: NON eseguiti qui.

## Ripristino del solo esperimento

Per usare il comportamento precedente basta avviare normalmente o in `native`.
Per rimuovere le modifiche ai sorgenti:

```powershell
python .\restore_spike_transport.py 'C:\BlenderDev\blender\spike-transport-backup-...'
.\COMPILA.ps1
```

Il ripristino modifica solo il sorgente spike e rimuove il suo header. Si ferma
se questi file sono cambiati dopo l'applicazione; non ripristina vecchie versioni
degli altri file FaceSets/layer della build.
