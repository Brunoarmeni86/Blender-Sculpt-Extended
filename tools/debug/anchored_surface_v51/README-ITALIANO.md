# Anchored Surface Projection V5.1 — stabilita e ottimizzazione

Base GitHub V5: `7320b6b5c8dde92074a66e5bc27e63500d77177c`.
Ramo nuovo: `feature/anchored-surface-projection-v51`.

## Applica e compila

Estrai lo ZIP e, dalla cartella estratta nella Developer PowerShell VS2022:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\APPLICA-E-COMPILA-ANCHOR-V51.ps1 -Run
```

Lo script riconosce V4, V4.1 o V5 e applica UNA delle tre patch alternative.
Non devi prima installare V5. Le patch non vanno applicate in sequenza.
Sorgenti predefiniti: `C:\BlenderDev\blender`.
Build: `C:\BlenderDev\build_windows_x64_conform_test`.
Eseguibile: `C:\BlenderDev\build_windows_x64_conform_test\bin\Release\blender.exe`.
La directory stabile `build_windows_x64_shareable` viene rifiutata.
Backup dei due file prima della modifica; nessun reset, pull o reconfigure.
Sono supportati anche sorgenti estratti senza .git. Con .git viene creato un ramo locale.
Messaggi di git su patch non applicabili durante l'autodetezione sono attesi.

Attiva gli stessi toggle: Anchored + Area Plane + Surface Projection + Geodesic + Transport.
La variabile resta `BLENDER_ANCHOR_PROJECTION_V5`: 1 usa V5.1, 0 usa il fallback V4
nella stessa build, sempre senza i vecchi limiti di campioni. `-Legacy` avvia quel confronto.
`-DiagnosticLevel 1` aggiunge il log; 2 include l'audit seam/tangenti.
`-ApplyOnly` applica senza compilare. Per la prima prova visiva usa diagnostica spenta.

## Cambiamenti rispetto a V5

* Controlli alfa raggruppati in blocchi locali 8x8. Un campione bilineare interno
  a un blocco usa una ricerca hash e quattro accessi diretti. I blocchi restano
  sparsi: nessuna tabella dimensionata per tutta la mesh o una griglia intera.
* Occupazione esplicita per ogni controllo: un punto mancante o irraggiungibile
  non viene interpretato come UV zero o come superficie valida.
* Indici del lattice tramite shift e maschere, con fattori precalcolati.
* Una passata grossolana viene interrotta appena emerge una necessita di
  raffinamento, invece di completare tutta la ricerca destinata a essere scartata.
* Pesi metrici degli edge precalcolati. Il solver iterativo viene saltato se
  l'errore metrico medio iniziale e gia sotto 1e-12.
* Normalizzazione difensiva contro normali e assi non finiti; controllo degli
  indici del campionatore e di raggi non finiti o fuori scala.
* Riuso della cache condizionato anche a dimensioni CCG, conteggio campioni e
  indirizzi dei buffer di posizioni/normali. La diagnostica riporta `layout_changed`.
  Non e un hash della geometria: le scritture in-place seguono ancora il ciclo
  della superficie di riferimento Anchored/Undo della V5.
* Dopo una reale std::bad_alloc, le cache vengono liberate e la preview usa V1/V2
  per il resto dello stroke, senza ritentare grandi allocazioni a ogni evento.
  Il tentativo riparte con una nuova impronta. Nessun tetto di campioni reintrodotto.

Restano la proiezione adattiva, la correzione metrica, i duplicati CCG canonici,
simmetria speculare/radiale/tiling e le cache distinte per passata della V5.
Nessuna modifica ai dati persistenti Sculpt Layers, Undo, Commit o Edit Mode.
L'interpolazione e il raffinamento mantengono i limiti qualitativi della prima V5:
non e una parametrizzazione esatta e non garantisce una mappa globale senza overlap.

## Verifiche e prestazioni

Kernel estratti compilati GCC C++17, con API Blender simulate. Passano cuciture
ruotate, fogli scollegati vicini, mirror X, radial 90 gradi, resize/rotazione,
raffinamento di pieghe e controllo dell'energia metrica. Rimossi ancora i cap:
V4 completa 103.041 frame e V3 314.721 campioni.

Passano anche controlli mancanti, indici non validi, raggio infinito, normali NaN,
sostituzione dei buffer e soppressione dei tentativi dopo errore di allocazione.
Verifica AddressSanitizer/UndefinedBehaviorSanitizer senza errori sui casi V5.1;
leak detection disabilitata per limitazioni del runtime.

Microbenchmark locale su piano sintetico, 2 milioni di campioni per esecuzione,
mediana di cinque esecuzioni alternate (API simulate):

| Tempo | V5 | V5.1 |
|---|---:|---:|
| Costruzione | 2.068 ms | 2.158 ms |
| Campionamento | 55.635 ms | 54.322 ms |

Il checksum dei risultati e identico. Il vantaggio di campionamento e modesto
in questo caso e la costruzione non migliora: questi numeri non dimostrano
un'accelerazione complessiva in Blender. I dati completi sono in `microbenchmark.json`.
L'arresto anticipato del raffinamento elimina lavoro scartato, ma non e stato
quantificato qui con un benchmark Blender reale.

NON compilata qui la build completa Blender/Visual Studio 2022.
NON eseguito qui lo script PowerShell. Qualita, tempi e memoria sulle tue
Multires e integrazione nel brush completo restano da verificare sul PC.

Test minimo: una alfa asimmetrica su naso/guancia, simmetria X e resize.
Se il risultato convince, passa all'orecchio concavo; un log diagnostico breve
puo distinguere costo di ricerca, correzione metrica e fallback.
