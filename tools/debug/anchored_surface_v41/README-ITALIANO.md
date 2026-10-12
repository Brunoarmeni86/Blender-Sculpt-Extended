# Anchored Surface Projection — diagnostica V4.1

Base reale GitHub: `feature/anchored-surface-transport-v4`, commit
`0abe66b8ab9b866d28da35412b56d576690e89ed`.
Ramo nuovo: `feature/anchored-surface-diagnostics-v41`.

## Applicazione Windows

Estrai il pacchetto. Nella Developer PowerShell di Visual Studio 2022,
esegui dalla cartella estratta:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\APPLICA-E-COMPILA-ANCHOR-V41.ps1 -RunDiagnostics
```

Sorgenti predefiniti: `C:\BlenderDev\blender`.
Compilazione incrementale: `C:\BlenderDev\build_windows_x64_conform_test`.
Eseguibile: `C:\BlenderDev\build_windows_x64_conform_test\bin\Release\blender.exe`.
La directory `build_windows_x64_shareable` e le sue sottocartelle sono rifiutate.
Lo script salva i due file prima di modificarli, controlla la patch,
crea un ramo locale e non esegue reset, pull o commit automatici.
Non eseguirlo dopo aver gia scaricato i sorgenti modificati dal nuovo ramo:
in quel caso la diagnostica e gia presente e basta compilare.

Solo applicazione: aggiungi `-ApplyOnly`.
Audit cuciture: aggiungi `-RunDiagnostics -DiagnosticLevel 2`.
La patch conserva integralmente il comportamento V4, inclusi i fallback esistenti.

## Test minimo sul tuo PC

Usa Draw con alfa asimmetrica (griglia o lettere), Anchored, Area Plane,
e abilita Surface Projection, Geodesic e Transport come nella V4.
1. Una sola impronta su guancia/naso, inizialmente senza simmetria.
2. Allarga e restringi la stessa impronta per circa 5 secondi.
3. Una impronta che attraversa una cucitura CCG con livello diagnostico 2.
Chiudi Blender e restituisci il log. Per un confronto visivo, riavvia
senza `BLENDER_ANCHOR_DIAGNOSTICS`: i contatori atomici attivi possono rallentare
il campionamento, quindi non usare il tempo complessivo della pennellata come benchmark.
Non servono test massivi di Undo/Commit per questa patch di sola diagnostica.

## Lettura del log

Ogni riga `[ANCHOR_DIAG]` descrive una passata di una preview Anchored.
I contatori `planar/v1/v2/v3/v4/rejected` contano chiamate al campionatore,
non vertici unici, pixel dell'immagine o impronte definitive.
`v2` indica una correzione normale effettivamente calcolata;
`v1` indica il ramo V1, anche dove le sue condizioni non producono correzione.

| Campo | Significato |
|---|---|
| `reason=built/reuse` | Cache costruita o riutilizzata |
| `reason=sample_cap_v1_v2` | Limite raggiunto; tutta la cache viene scartata, anche la V3 |
| `transport` | Abilitazione, esclusione per simmetria/tiling, normali/assi invalidi |
| `cache` | Motivo di ricostruzione: raggio, origine, asse, passata o geometria |
| `requested/built` | Toggle Transport richiesto / costruzione frame avviata; built non garantisce copertura completa |
| `prepare_ms` | Preparazione totale, inclusi ricerca seed, allocazioni e audit se attivo |
| `search_ms` | Dijkstra e costruzione frame; zero sul riuso |
| `audit_ms` | Costo aggiuntivo dell'audit livello 2 |
| `visited/frames` | Campioni scoperti / frame UV presenti (anche fuori dal raggio effettivo, fino a 1.35R) |
| `cap` | 100.000 con Transport; 300.000 senza |
| `popped/relaxed` | Estrazioni dalla coda / aggiornamenti accettati, non campioni unici |
| `transport_fail` | Aggiornamenti senza frame genitore o con rotazione rifiutata |
| `uv_nonfinite` | Frame rimossi per UV non finite |
| `seam_edges` | Aggiornamenti accettati che cambiano griglia |
| `seam_pairs/seam_missing` | Coppie coincidenti tra griglie con due frame / secondo frame assente |
| `seam_uv_max` | Massimo salto UV in unita oggetto; dividere per radius per normalizzarlo |
| `tangent_error` | Massimo valore assoluto del prodotto normale-tangente |
| `seed_normal_dot/seed_y_dot` | Allineamento iniziale normale / asse Y con il frame del brush; negativo segnala possibile inversione |
| `missing_ccg` | Chiamate Area Plane senza indice CCG: non possono usare V3/V4 |

I valori dell'audit e dell'orientamento seed sono zero sul riuso: non vengono ricalcolati.
L'audit considera adiacenze topologiche CCG coincidenti entro 1e-8 unita oggetto,
non punti vicini individuati tramite ricerca spaziale. Non controlla tutte le
distorsioni interne o gli overlap: zero salto cucitura non significa mappa perfetta.

## Stato di validazione

Implementati: contatori opzionali, motivi fallback/ricostruzione, tempi, audit seam e assi.
Verifiche locali e limiti: vedi `VALIDAZIONE.md`.
La compilazione completa Blender con Visual Studio 2022 e la validazione visiva
devono avvenire sul tuo PC. Nessuna nuova proiezione V5 e inclusa nella diagnostica.

La proposta successiva e in `PROGETTO-V5.md`.
