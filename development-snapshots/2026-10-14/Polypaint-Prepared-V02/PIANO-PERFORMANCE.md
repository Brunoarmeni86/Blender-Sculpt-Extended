# Avanzamento delle ottimizzazioni Paint

Prepared V02 implementa il primo passo: piano per dab, puntatori riusati dentro il nodo, eliminazione della seconda lettura del colore e dei controlli ripetuti nel writer. Quando Paint e master hanno pari risoluzione evita le otto scritture con peso zero. La prima scrittura di ogni griglia resta sul riferimento e prepara il percorso successivo. Non cambia ordine o raggruppamento dei campioni.

Le misure native distinguono raccolta posizioni, fattori e scrittura, con tempi di preparazione inclusi nella scrittura. La durata Python della pennellata comprende anche lavoro esterno al kernel. Il costo specifico di Undo o GPU non e attribuito artificialmente a queste fasi.

Prossimi passi dopo verifica su mesh dense:

1. Usare i log per scegliere se il limite prevalente e fattori, scrittura, prima materializzazione, Undo o aggiornamento visivo.
2. Parallelismo tra griglie con proprieta esclusiva, verificando l'assegnazione dei nodi e i bordi condivisi. Scratch per thread, nessuna promozione o riallocazione globale dai worker. Conservare l'ordine interno alla griglia e un riferimento seriale selezionabile.
3. Trasferimento a livelli inferiori per blocchi soltanto con un riferimento numerico affidabile: le scritture correnti influenzano le letture successive, quindi una somma ingenua di delta precalcolati non e equivalente.
4. Se necessario, Color Undo per blocchi sporchi e upload GPU per regioni, con identita attributo/risoluzione, presenza delle griglie implicite e invalidazione delle cache correttamente conservate.
5. Misurare le copie dei master operate da depsgraph e Memfile Undo prima di introdurre viste condivise con una nuova gestione della durata dei payload.

I rapporti del benchmark isolato non sono promesse di velocita della pennellata in Blender. Una modifica per kit, con confronto A/B e test multi-attributo.
