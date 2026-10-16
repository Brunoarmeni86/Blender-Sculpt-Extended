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
