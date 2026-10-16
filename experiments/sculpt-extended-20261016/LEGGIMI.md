# Sculpt Extended checkpoint 2026-10-16

Aggiornamento del branch `checkpoint/sculpt-extended-20261015`, a partire dal
checkpoint `8c8f0d3219b`. Il branch `main` non viene modificato.

Lo script legge i sorgenti effettivi in `C:\BlenderDev\blender` e copia solo i 15
file nativi relativi alle modifiche successive al checkpoint, inclusi i quattro
nuovi header. Il resto del progetto e gia presente nel checkpoint precedente.
Salva inoltre il kit completo di avvio attualmente installato nella shareable,
con un wrapper aggiornato, in `experiments/sculpt-extended-20261016`.

Il profilo predefinito abilita MultiCache V01 con limite 1024 MiB, Overlay Neutral
V01 e Open File Undo V01. Mantiene Geometry Buffer, Delta Smooth, Polypaint,
bake UV/UDIM e le altre opzioni del kit precedente. Il fattore PBVH e 1.
Il probe di raggruppamento e la diagnostica restano nel codice della build,
ma non vengono attivati come nuove ottimizzazioni nel profilo predefinito.
I log aggiuntivi Cold Start / Stroke / Draw sono disattivati.

Verifica utente: MultiCache e Overlay Neutral funzionano nei test eseguiti;
il nuovo primo Undo dopo l'apertura del file e stato riferito funzionante.
Il leggero aggiornamento iniziale della viewport rimane. Nei log precedenti
gli invoke Crease erano circa 17 ms, mentre gli Undo verso Original richiedevano
18.5 / 9.1 s. Non sono disponibili nuovi tempi quantitativi dopo la correzione.
Il guadagno del raggruppamento PBVH 2 rispetto a 1 non e dimostrato.
La doppia valutazione iniziale del Multires resta da ottimizzare.

## Esecuzione sul PC di sviluppo

Estrarre la cartella `Sculpt-GitHub-Checkpoint-20261016` in `C:\BlenderDev`,
chiudere Blender e attendere eventuali build, quindi eseguire:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\BlenderDev\Sculpt-GitHub-Checkpoint-20261016\AGGIORNA-GITHUB.ps1"
```

Usa il repository Git esistente `C:\BlenderDev\Blender-Sculpt-Extended-publish`.
Non clona nuovamente Blender, non copia librerie, file blend, binari, log o backup.
Non aggiunge eliminazioni di asset/test o file estranei gia presenti nel repository.
I submodule non vengono attraversati. Non usa reset, clean o push forzati.
La cartella dei sorgenti di lavoro e la build vengono soltanto lette.

Prima della copia controlla branch, origin, identita Git, staging vuoto, base
comune e aggiornamento remoto. Modifiche gia presenti nei sorgenti del repository
di pubblicazione fanno interrompere l'operazione. Non le sovrascrive.
Salva un backup delle destinazioni esistenti, verifica SHA256 dopo ogni copia,
crea il commit, esegue il push e confronta lo SHA locale con quello remoto.
La pubblicazione e conclusa soltanto quando compare `PUBBLICATO E VERIFICATO`.

Il file CHECKPOINT-FILES.json registra gli hash dei file copiati e dell'eseguibile
osservato. Non certifica automaticamente che quell'eseguibile sia compilato da
quei sorgenti: le prove di compilazione e funzionamento sono quelle eseguite
dall'utente sulla build Windows.

## Avvio dopo una futura installazione nella shareable

Copiare il contenuto del kit salvato in `bin\Release\SculptExtended`, conservando
la struttura delle sottocartelle. Il wrapper completo risolve l'eseguibile accanto
alla cartella SculptExtended; accetta anche `-BlenderExe` e `-BlendFile` espliciti.
Questo aggiornamento GitHub non copia automaticamente il kit sulla build attuale.



## Validated startup reuse V02 (2026-10-16)

File-load restoration checks the completed Multires CCG before temporarily
resetting the saved Sculpt mode and tagging SYNC_TO_EVAL. The active, editable,
visible object's existing evaluation is retained only when the complete strict
preflight succeeds. Sculpt session initialization validates again after tool
settings initialization and falls back to native geometry evaluation if needed.
The fully evaluated graph guard and CCG ownership are retained. Other restore
paths are unchanged. BLENDER_MULTIRES_STARTUP_REUSE=1 enables this in the full
launcher; unset/0 follows native restoration. The validated initial Undo anchor
remains enabled.

Windows user build and log comparison on mostro.blend: 124208 grids, area 289.
Cold-Start-20261016-075500.zip: two CCG builds, Sculpt init 10832.850 ms.
Cold-Start-20261016-080816.zip: one CCG build, Sculpt init 1270.905 ms,
mode=REUSE with identical candidate/live pointer. Recorded Undo 4.911 ms.
This measures Sculpt initialization, not total file opening. GPU preparation
varies between runs and is not an isolated benchmark of this change.

Local C++17 ASan/UBSan tests use simulated Blender APIs: 42 predicate rejection
cases plus the actual added restore branch and its eligibility/fallback cases,
including invalidation after toolsettings. Complete Blender Windows compilation
and runtime confirmation were performed by the user; broader scene compatibility
is not established by this one load test.

The CHECKPOINT-FILES manifest retains previous hashes for unchanged files.
Changed and new file hashes in this commit use the Git UTF-8 file content.
No hash for the latest user-built executable was supplied in this update.



## Validated full CCG batch V01 (2026-10-16)

Full displaced CCG evaluation can use the existing OpenSubdiv multi-face batch
implementation with up to eight coarse faces per batch. Quad/ngon coordinate
mapping, displacement, mask writes and the native final normal stage are retained.
The scalar zero/equal derivative correction is explicitly preserved for the new
full path; existing local batch calls retain their previous default behavior.

BLENDER_MULTIRES_CCG_FULL_BATCH=1 enables this in the complete launcher. Unset/0
follows native scalar evaluation. Oversized individual faces, missing evaluators
and incompatible layouts fall back. Temporary owned scratch is capped at 1 MiB
per worker, targeting 16 MiB divided over reported CPU threads; OpenSubdiv internal
temporary allocations are additional. No persistent dense cache is added.

The user confirmed retaining the change. Windows mostro.blend log comparison,
124208 grids, area 289:
- previous Cold-Start-20261016-080816.zip positions 4720.192 ms, grid evaluation
  5038.520 ms, conversion to CCG 7180.618 ms;
- Cold-Start-20261016-082324.zip positions 2558.033 ms, grid evaluation
  2821.855 ms, conversion to CCG 5033.468 ms; one CCG build and Startup V02 REUSE;
- recorded Undo/Redo 6.248-15.976 ms; own scratch 469.6 KiB per worker.
Cold-Start-20261016-082404.zip contains identical session files and is not an
independent repeat. This is one before/after scene test, not broad compatibility
or a total file-open benchmark.

Local C++17 ASan/UBSan tests with simulated Blender/evaluator APIs compare
24 mixed tri/quad/ngon configurations at levels 1-6 with four threads: scalar/batch
coordinates and masks match bit-for-bit, including singular-derivative correction.
Normal-stage inputs and fallback/opt-in behavior are checked; actual OpenSubdiv
and full Blender Windows validation require runtime tests. The user compiled
and ran this Windows build. Latest executable hash was not supplied.
