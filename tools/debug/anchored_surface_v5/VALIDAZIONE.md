# Verifiche V5

La verifica e un piccolo eseguibile GCC C++17 che compila le vere funzioni
di costruzione, trasporto, interpolazione e correzione estratte da sculpt.cc.
Le API Blender sono simulate: non e una build di Blender.

| Caso | Risultato locale |
|---|---|
| Cucitura tra griglie ruotate di 90 gradi | UV identiche sul duplicato |
| Foglio scollegato a 0.1 unita dalla superficie | Rifiutato anche dentro il raggio spaziale |
| Alfa asimmetrica su piano, simmetria X | U riflessa e V conservata |
| Rotazione radiale 90 gradi | U/V orientate come gli assi trasformati |
| Ritorno alla passata principale dopo speculare | Riutilizzata la stessa cache |
| Riduzione raggio e rotazione alfa | Riutilizzata la stessa cache |
| Griglia 321x321 = 103.041 campioni | 1.681 controlli, stride 8; UV high-res interpolate corrette sul piano |
| Piega geometrica in lattice ridotto | Raffinamento fino a stride 1 |
| Patch curva | Energia strain 7.91677929e-5 -> 6.3311168e-5, cinque passi accettati |
| Fallback V4 con V5 disabilitata | 103.041 frame completati, nessun limite 100.000 |
| Fallback V3 senza Transport | 314.721 campioni completati, nessun limite 300.000 |

Controllati anche diagnostica 0/2, formati printf e assenza di errori whitespace
con git diff --check. I casi V5 passano anche con AddressSanitizer e
UndefinedBehaviorSanitizer; leak detection disabilitata perche il runtime
non permette a LeakSanitizer di ispezionare i thread via /proc. Verificate le due patch alternative in avanti e al contrario.
Letta l'API reale SubdivCCGNeighbors::duplicates() e controllata la configurazione
Windows del repository per supporto /EHsc.

Questi dati non sono benchmark Multires. La riduzione di energia sintetica
non equivale a una percentuale di miglioramento visivo su naso o orecchie.
Non testati qui: processo Blender completo, PBVH/depsgraph reali, input Anchored,
alfa nel brush completo, Sculpt Layers/Undo/Commit, layout UI, esecuzione PowerShell
e tempi/memoria sul PC Windows. Le modifiche riguardano soltanto la proiezione
e cache temporanee dello stroke; non scrivono dati di Sculpt Layers o topologia.

La validazione sul PC puo restare minima: una alfa asimmetrica su guancia/naso,
simmetria X e resize; se il risultato convince, prova l'orecchio concavo.
