# Sculpt Extended — architettura sperimentale V01

Stato: progetto, non implementazione. Baseline congelata: V05,
`b63dd98c27ae8d636046cf18cd54b7ed967f9cc8`.
Ramo: `experiment/sculpt-sparse-blocks-20261021`.
La portable completa in compilazione resta indipendente da questo lavoro.

## Obiettivo e limite fisico

Ridurre memoria, copie e lavoro proporzionale all'intera superficie quando un
layer modifica solo una regione. Non promettere un'accelerazione equivalente
della suddivisione nativa: base Multires, valutatore e superficie visualizzata
possono restare densi. La prima apertura di un layer e il cambio di visibilità
vanno misurati separatamente dalla suddivisione.

Il fork ha già griglie di layer con payload nullo e un indice delle griglie
attive. Il passo nuovo è rendere sparso anche l'interno di una griglia.
Il numero di campioni CCG, incluse le duplicazioni ai bordi, non coincide con
il numero di poligoni. Un array float3 di 160 milioni di campioni occupa circa
1,79 GiB: questo costo si moltiplica per ogni copia densa temporanea.

## Contratti da conservare

- Stessa geometria entro la tolleranza già verificata; nessun cambio intenzionale
  nell'ordine di composizione dei layer o nelle operazioni floating point.
- Visibilità, intensità negativa e nulla, gruppi, maschere e layer nascosti
  conservano il comportamento attuale. La visibilità non cancella dati.
- Livello zero e dettaglio HR restano distinti: i delta della gabbia cambiano
  anche i riferimenti tangenti; non sono semplicemente sommabili ai delta HR.
- Face Sets e Polypaint conservano dati, algoritmi e interfacce attuali.
- Errori di allocazione o validazione non pubblicano modifiche parziali.
- Nessuna modifica sperimentale entra nella release congelata automaticamente.

## Rappresentazione proposta

Una superficie layer contiene una directory di griglie. Ogni griglia usa blocchi
2D immutabili di delta float3 oppure un payload denso. Il prototipo confronta
blocchi 16×16, 32×32 e 64×64: rispettivamente 3, 12 e 48 KiB a pieno carico,
esclusi metadati. I blocchi al margine conservano solo i campioni validi.
La dimensione definitiva viene scelta dai risultati, non fissata a priori.

Un blocco assente significa delta esattamente zero. Un blocco presente conserva
tutti i suoi campioni, senza quantizzazione né soglie che eliminino dettagli.
Anche i valori subnormali finiti devono sopravvivere; la verifica di zero deve
essere indipendente da eventuali modalità floating point FTZ/DAZ.
Metadati di occultamento e maschere hanno proprietà separata dal payload:
non possono sparire quando un blocco geometrico diventa nullo.

La chiave logica comprende identità del layer, corner persistente, livello e
generazione della topologia/riferimento. I puntatori e i nomi non sono identità
persistenti. Gli indici correnti della mesh si risolvono attraverso gli attributi
corner/vertex già presenti; identificatori mancanti o duplicati richiedono
validazione e il percorso esistente di rimappatura.

La scelta sparso/denso è per griglia, con isteresi per evitare conversioni
continue. Soglie e costo di conversione vanno misurati includendo directory,
allocatore, Undo e memoria transitoria. Un layer che copre tutta la mesh deve
poter mantenere una rappresentazione densa efficiente.

## Modifica, transazione e Undo

Durante uno stroke si crea una transazione delle sole regioni modificate.
I blocchi condivisi vengono copiati alla prima scrittura; la directory usa
pagine condivise o una struttura equivalente, evitando la copia dell'intera
mappa a ogni stroke. Al termine si validano identità, dimensioni, valori e
revisioni; poi si pubblica la nuova radice. Un errore scarta la transazione.

Undo mantiene le radici e i payload necessari alle versioni raggiungibili.
Questo non rende gratuito tutto l'Undo di Blender: coordinate native, CCG,
maschere e metadati continuano a usare i percorsi appropriati. La prima
integrazione deve agganciarsi alla cattura esistente, senza registrare due copie
complete dello stesso layer. Compressione e reclamazione dei blocchi si valutano
solo dopo aver misurato il costo reale di snapshot e ripetuti Undo/Redo.

## Composizione e cache

Lettura per regione: base nativa, poi layer efficaci nello stesso ordine attuale.
I blocchi mancanti evitano il lavoro sul relativo delta. Strength e visibilità
invalidano la composizione, non riscrivono i payload del layer.

La cache composta è limitata da un budget esplicito. Le chiavi includono livello,
topologia, revisione della gabbia/riferimenti, base, payload e parametri dei layer.
Una revisione incompatibile invalida il risultato; non si riutilizza una cache
soltanto perché l'indirizzo della mesh coincide. La pubblicazione delle revisioni
segue la sincronizzazione del percorso Sculpt, senza introdurre scritture
concorrenti non protette.

Aggiornamento della viewport: notificare le regioni PBVH coinvolte e la chiusura
delle dipendenze effettive, inclusi i bordi. Non assumere che basti sempre un
anello di vicini. Non si propone riduzione della qualità visiva o LOD nella prima
versione. Se l'operazione cambia tutta la superficie, il lavoro può restare globale.

## Livelli e Subdivide

Prima si integra lo storage senza cambiare la matematica Multires. Successivamente
si separano allocazione, raffinamento, ricostruzione dei riferimenti e composizione.
Il raffinamento dei delta sparsi deve seguire il supporto reale dell'operatore:
una regione non nulla può influenzare campioni confinanti. La chiusura va ricavata
dal codice e confrontata con l'esecuzione densa, anche per bordi e topologie
straordinarie. Nessun salto di griglia basato soltanto sull'assenza di input locale.

Il livello zero continua a usare `base_deltas` e i percorsi di rebase attuali.
La prima integrazione può usare un adattatore denso per una griglia alla volta.
Le operazioni non ancora portate mantengono il percorso consolidato; i costi di
materializzazione vengono registrati. Non mantenere permanentemente due copie,
sparsa e densa, per simulare un miglioramento.

## Salvataggio e compatibilità

Fase iniziale: storage sperimentale solo runtime, lettura del formato attuale e
salvataggio tramite conversione controllata al formato esistente. La memoria
aggiuntiva di questa conversione è una limitazione dichiarata e misurata.
Il formato corrente scrive strutture MDisps e array per griglia: un salvataggio
a blocchi richiede progettazione e versionamento separati, non solo nuovi puntatori.
Verificare esplicitamente anche persistenza dei metadati hidden e copia modifier.

Fase successiva, solo dopo i test runtime: formato versionato con controlli di
dimensioni, overflow, riferimenti e dati corrotti; importazione dei vecchi file;
nessuna migrazione distruttiva automatica del progetto originale.

Per Blender standard si esporta una COPIA con layer visibili incorporati nel
Multires nativo e componenti proprietarie rimosse. Il progetto editabile conserva
i layer. Il bake usa i percorsi corretti per gabbia, riferimenti tangenti e livelli;
non basta sommare array arbitrari. L'output nativo resta denso. La compatibilità
va provata aprendo davvero la copia con Blender standard, confrontando geometria
e aspetto. Il colore HR richiede il proprio percorso di esportazione: non si
presume che un Color Attribute base conservi tutti i campioni HR.

## Punti del codice coinvolti

| Area | Punto attuale | Conseguenza |
| --- | --- | --- |
| Proprietà layer | `DNA_modifier_types.h`, `MultiresSculptLayer` | Nessun cambio DNA nella prima prova |
| API e identità | `BKE_multires_sculpt_layers.hh` | Riutilizzare corner ID e rimappatura |
| Composizione e bake | `multires_sculpt_layers.cc` | Conservare ordine, strength e contratti L0/HR |
| Persistenza/copia | `MOD_multires.cc` | Adattatori e audit di proprietà degli array |
| Undo Sculpt | `sculpt_undo.cc` | Integrare snapshot senza duplicazione del payload |
| Suddivisione V05 | pipeline compact e cache evaluator | Baseline di confronto; ottimizzazione distinta |

La lettura svolta identifica questi punti di integrazione; non equivale ancora
a un audit completo di tutti i consumatori di `layer.disps`.

## Consegne e criteri di passaggio

1. Prototipo C++ isolato: directory, blocchi, transazioni, snapshot, fallback denso.
   Nessuna dipendenza da file Blender o UI. Confronto con riferimento denso su
   dati casuali deterministici, bordi, zeri, subnormali, edit ripetuti e rollback.
2. Audit dei consumatori, poi una sola integrazione Draw sul layer selezionato,
   dietro flag disattivato normalmente. Nessuna riscrittura dei pennelli insieme
   allo storage. Confronto geometrico e Undo/Redo con la release congelata.
3. Copertura di composizione, visibilità, strength, bake, livelli, subdivide e
   salvataggio/riapertura. Le operazioni scoperte restano sul percorso consolidato.
4. Solo con esito positivo: formato persistente ed esportazione stock verificata;
   poi estensione agli altri strumenti che condividono gli stessi contratti.

Un unico runner riassume correttezza e prestazioni per ogni consegna. Casi:
regione piccola, regione distribuita, copertura completa, più layer anche nascosti,
strength negativa/zero, L0 e HR, maschere, bordi, Undo/Redo, riapertura e fallimento
simulato di allocazione. Non serve ripartire dai test manuali generali dei pennelli.

Registrare separatamente: base nativa, payload, directory, cache, scratch,
valutatore e Undo; memoria impegnata e picco residente; tempi di primo accesso,
stroke, visibilità e Subdivide. Confronti ripetuti nella stessa sessione e con
ordine alternato; i guadagni devono superare il rumore osservato. Accettazione:
correttezza invariata, risparmio netto nei casi sparsi e assenza di regressioni
materiali nei casi densi. Le soglie numeriche si fissano sui dati del prototipo.

## Fuori dalla prima versione

GPU, out-of-core, LOD, nuovo sistema Face Sets/Polypaint e trasporto con cancellazione
atomica della sorgente sono progetti distinti. La prima decisione da prendere
con misure è se il risparmio dei blocchi giustifica complessità e metadati, non
se sostituire subito tutto il Multires.
