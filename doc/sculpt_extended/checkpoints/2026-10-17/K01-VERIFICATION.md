# K01 — preparazione del master Face Set durante Undo

Lo snapshot Face Set su Multires poteva chiamare `faceset_ensure_from_mesh`
dentro il lavoro parallelo. Il master del modifier è condiviso tra i nodi,
mentre il mutex della mappa Undo protegge soltanto la directory dei nodi.

La preparazione avviene una volta per step. `push_nodes` la esegue prima del
`parallel_for`; `push_node` usa `std::call_once` perché può essere chiamato
direttamente da worker diversi. Il risultato, incluso un fallimento, è
pubblicato prima delle copie dei buffer. La copia per nodo resta parallela;
non vengono aggiunte scansioni per vertice o serializzate le copie grandi.

Test C++ locale: 64 worker simultanei, una sola inizializzazione, percorso
batch, fallimento stabile per step e nuovo tentativo in uno step successivo;
ASan/UBSan PASS. Si tratta del corpo helper reale con adattatori per il
master e lo scheduler, non di una build completa di Blender.

Test Windows `verifica-20261017-061147-729.log`, SHA256
`0327eb87ea5a263cf36499e2f909e501480379cb8187804bd78102906f00e8b3`.
PASS: due scene factory 49×49; Create usa il percorso `push_node`
parallelo, Init usa `push_nodes`; Undo/Redo riusciti. La fixture
[TEST-IN-BLENDER.py](../../../../experiments/sculpt-consolidated-20261017/tests/faceset-undo/TEST-IN-BLENDER.py)
non può ispezionare il master HR via RNA e una sessione senza crash non
dimostra formalmente l'assenza di race. Mancano un test ThreadSanitizer e
pennellate Face Set prolungate su mesh grandi. Nessun benchmark numerico.

Le segnalazioni di proprietà RNA con default stringa vuota all'avvio
precedono questa patch e restano separate. Il commit GitHub ricomposto non
ha ricevuto una build MSVC completa distinta dagli ZIP verificati.
