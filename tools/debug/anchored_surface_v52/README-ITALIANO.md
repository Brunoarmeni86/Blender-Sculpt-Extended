# Anchored Surface Projection V5.2 — centratura e pieghe interne

Base: V5.1 validata visivamente dall'utente, commit GitHub
`4dc642d17499ee4403945564c37a4850015d7acf`.
Nuovo ramo: `feature/anchored-surface-projection-v52`.
La V5.1 rimane disponibile per il confronto.

## Applicazione Windows

Estrai e, nella Developer PowerShell VS2022 dalla cartella estratta, esegui:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\APPLICA-E-COMPILA-ANCHOR-V52.ps1 -Run
```

Lo script sceglie UNA patch da V4, V4.1, V5 o V5.1. Non applicarle in sequenza.
Sorgenti: `C:\BlenderDev\blender`.
Build: `C:\BlenderDev\build_windows_x64_conform_test`.
Eseguibile: `C:\BlenderDev\build_windows_x64_conform_test\bin\Release\blender.exe`.
La build `build_windows_x64_shareable` viene rifiutata. Backup dei due sorgenti;
nessun reset, pull o reconfigure. Sono supportati sorgenti con e senza .git.

I tentativi di riconoscimento delle patch sono ora silenziosi: restano visibili
l'esito finale e tutti gli errori effettivi della compilazione.
Il lancio predefinito attiva V5.2. Usa gli stessi toggle nel brush:
Anchored + Area Plane + Surface Projection + Geodesic + Transport.

Confronto V5.1 nella stessa build:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\APPLICA-E-COMPILA-ANCHOR-V52.ps1 -Run -V51
```

`-Legacy` sceglie V4, `-DiagnosticLevel 1` registra un log e 2 aggiunge gli audit.
`-ApplyOnly` applica senza compilare. Variabili ripristinate dopo la chiusura.
Per avvio manuale, `BLENDER_ANCHOR_PROJECTION_V5=1` e
`BLENDER_ANCHOR_PROJECTION_V52=1` selezionano V5.2; mettere la seconda a 0
mantiene il percorso V5.1. Le impostazioni vengono lette all'avvio del processo.

## Cambiamenti reali

1. **Sorgente continua.** Il centro non viene piu trattato come un singolo nodo
   arrotondato del lattice. Viene adattato localmente al dominio bilineare CCG,
   considerando anche i duplicati topologici del punto hit sulle cuciture.
   Non e una ricerca spaziale che salda superfici vicine. Un residuo maggiore
   dell'1% del raggio disabilita questa sorgente, preservando l'aggancio
   precedente per origini di simmetria/tiling lontane dalla superficie.
2. **Connessioni multiple.** Fino a quattro controlli circondano la sorgente.
   Posizioni, normali e trasporto sono integrati sul breve percorso locale;
   quei controlli restano ancorati durante la correzione metrica.
3. **Ricentratura UV.** Dopo la correzione, si sottrae l'UV interpolata al centro
   dalla patch. La traslazione non cambia la metrica in aritmetica esatta.
4. **Correzione radiale.** Interpolare direttamente le distanze dei quattro
   angoli rendeva positiva la distanza anche al centro. Si interpola invece
   il residuo rispetto alla lunghezza UV, con residuo nullo nella cella radice.
   La distanza al centro torna a zero; resta il fallback V3 per frame mancanti.
   Questa e una correzione locale: il campo geodetico di grafo rimane approssimato.
5. **Sonde interne.** Fino a nove punti interni di ciascuna cella grossolana
   visitata verificano l'errore rispetto alla posizione bilineare prevista.
   Una piega tra edge/diagonali puo quindi obbligare a raffinare il lattice.
6. **Isteresi sul resize.** Quando bisogna ricostruire la copertura, si mantiene
   lo stride precedente finche quello naturale non e almeno quattro volte
   maggiore. Si riducono i cambi di lattice alle soglie intermedie, senza
   impedire il coarsening su impronte molto grandi. Questo puo mantenere fino
   a quattro volte piu controlli rispetto alla scelta immediata per una singola
   soglia 2x; non e una promessa di resize piu veloce in ogni caso.

Restano simmetria speculare/radiale/tiling, blocchi sparsi 8x8, fallback esistenti,
controlli di stabilita e gestione dell'allocazione fallita della V5.1.
Nessun limite di 100.000/300.000 campioni reintrodotto e nessuna scrittura
dei dati di Sculpt Layers, Undo, Commit, Edit Mode o della topologia.

## Verifiche eseguite

Kernel veri estratti e compilati GCC C++17 con adattatori CCG/PBVH simulati:

| Caso sintetico | Profilo V5.1 | V5.2 |
|---|---:|---:|
| Distanza al centro fuori dal lattice, passo 8 | 6.94260788 unita | 0 |
| Piega tra edge/diagonali nella cella | Stride 8, non rilevata | Stride 1, raffinata |
| Crescita raggio 100 -> 150 | Stride 1 -> 2 | Stride 1 conservato |
| Ulteriore crescita a 300 | Stride 4 | Stride 4, coarsening consentito |

Passano inoltre centro frazionario sul piano, UV speculari, radial 90 gradi,
cuciture ruotate, superfici scollegate vicine, resize/rotazione e correzione
metrica su patch curva. I nuovi casi passano con ASan/UBSan; leak detection
disabilitata per le limitazioni del runtime. Le quattro patch alternative
sono verificate in avanti, al contrario e contro i byte dei sorgenti finali.
I dati dei nuovi casi sono in `quality-check.json`.

Un controllo di campionamento di 2 milioni di punti su piano sintetico ha
misurato circa 65.28 ms nel profilo V5.1 e 66.27 ms in V5.2. E una sola misura
con API simulate, non un benchmark Blender: questo passo privilegia la
qualita e non dimostra un guadagno di prestazioni. Sorgente continua, sonde
e correzione radiale aggiungono calcolo; il riuso delle cache resta importante.

## Limiti e stato

La V5.2 e implementata e verificata nel kernel isolato. NON e stata compilata
qui la build completa Blender/Visual Studio 2022, ne eseguito PowerShell.
Qualita visiva, memoria, input Anchored e prestazioni reali restano da verificare
sul PC. Nessuna garanzia di perfezione su tutte le superfici.

Il punto sorgente e un adattamento bilineare locale, non un hit analitico della
superficie limite; `source_error` nel log ne misura il residuo in unita oggetto.
`seed_count` indica quante connessioni virtuali sono state inizializzate.
Se l'adattamento non riesce, resta l'aggancio del percorso precedente.
Le sonde non esauriscono tutti i campioni interni: dettagli tra le sonde possono
ancora sfuggire. Il raffinamento resta uniforme per l'impronta, non per cella.
Una parametrizzazione locale puo conservare distorsioni, overlap o tagli su
volumi molto ripiegati; le distanze di grafo non sono geodetiche continue esatte.
L'isteresi riduce cambi di densita, ma non elimina ogni variazione della mappa
quando si ricostruisce un dominio piu grande.

Test minimo sul PC: stessa alfa asimmetrica su naso/guancia nelle due modalita,
poi resize lento e simmetria X. Conserva V5.1 se V5.2 non migliora il tuo caso.
