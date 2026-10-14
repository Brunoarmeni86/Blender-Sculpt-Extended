# Punto due: Paint Multires su mesh dense

La candidata multi-attributo conserva uno slot attivo per la scrittura. Il ciclo Paint legge quello slot senza cercare nella lista degli altri attributi. La selezione scambia i puntatori delle directory; non ricopia i payload nel kernel. Le copie effettuate da depsgraph e Undo vanno misurate separatamente.

Il percorso CPU attuale e seriale fra nodi. Ogni campione influenzato legge il colore e il writer lo ricampiona, ripete validazioni e risolve la griglia. Sotto la risoluzione del master, i supporti di add_coarse_delta si sovrappongono e aggiornano ripetutamente gli stessi campioni. Color Undo cattura le griglie dei nodi coinvolti alla risoluzione del master. Sono candidati colli di bottiglia ricavati dal codice, non tempi misurati sulla tua macchina.

Ordine di implementazione:

1. Misurare separatamente preparazione, fattori del brush, lettura/scrittura, allocazione/inizializzazione delle griglie, Color Undo, trasferimento colore alla GPU e copie di valutazione. Confrontare primo tratto e griglie gia dipinte, Sculpt=master e Sculpt<master, con gli altri attributi presenti ma inattivi.
2. Accesso preparato per griglia: attributo e risoluzione fissati prima del ciclo; puntatori e stride validati una volta; lettura/scrittura diretta a pari livello; interpolazione con coefficienti preparati negli altri casi. Conservare la sequenza matematica del writer originale.
3. Parallelizzare solo unita con proprieta esclusiva delle griglie. Audit dei nodi e dei campioni condivisi, assegnazione deterministica del lavoro e gestione dei bordi separata. Nessuna promozione o riallocazione globale dai worker. Scratch locale per thread. Confronto deterministico con il riferimento seriale e test delle cuciture.
4. Ottimizzare i supporti a livello inferiore. Un algoritmo per blocchi deve preservare la dipendenza fra la scrittura corrente e le letture dei campioni successivi; una semplice somma di delta precalcolati puo cambiare la pennellata. Implementare solo dopo un riferimento numerico affidabile.
5. Se le misure lo giustificano, Color Undo per blocchi sporchi salvati alla prima scrittura della pennellata, con identita attributo e risoluzione, e GPU update delle sole regioni modificate. Verificare presenza/assenza delle griglie implicite, bordi, Undo/Redo e invalidazione di tutte le cache.

Una modifica per kit, ciascuna confrontabile con la base corrente. Nessuna accelerazione numerica promessa prima di un benchmark nativo sulle tue mesh dense. GPU Paint e un intervento successivo, con sincronizzazione del master CPU e Undo da progettare separatamente.
