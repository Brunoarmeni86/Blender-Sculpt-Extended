# Quanto e migliorabile, e quale passo conviene

## Conclusione

Il margine e concreto: la V4 e un primo sviluppo discreto di coordinate tangenti,
non una parametrizzazione che minimizza la distorsione. Non e possibile stimare
una percentuale credibile di miglioramento visivo senza log e confronti sul modello.
Su patch piccole e lisce la V3 puo gia essere vicina al risultato utile;
sui cambi di curvatura di naso, zigomo e pieghe ci aspettiamo maggiore margine.
Su orecchie ripiegate, narici e superfici con percorsi geodetici concorrenti
una mappa unica e continua puo richiedere tagli o un'impronta piu piccola.
Una superficie con curvatura gaussiana non nulla non puo essere distesa sul piano
preservando contemporaneamente tutte le distanze, gli angoli e le aree.

## Riscontri nel codice V4, non ipotesi sui tuoi test

File: `source/blender/editors/sculpt_paint/mesh/sculpt.cc` alla revisione base.

* V4 viene campionata solo con Anchored + Area Plane + Surface + Geodesic +
  Transport, cache pronta, indice CCG e frame presente.
* Il trasporto viene costruito soltanto nella passata principale; simmetria e
  tiling usano V3 se la loro cache riesce a completarsi.
* V4 ha un limite di 100.000 indici CCG scoperti; V3 di 300.000. Sono inclusi
  duplicati di cucitura e l'area fino a 1.35 volte il raggio del brush.
* Superato il limite, la cache intera viene scartata: V4 NON ritenta V3.
  Si torna alla V1/V2. Questo potrebbe nascondere il miglioramento su Multires dense.
* Il riuso richiede stessa geometria, stessa origine entro 1e-6 unita oggetto,
  stessa passata, copertura >=1.05R e, per Transport, asse con dot >0.9999.
  Crescita sufficiente, rotazione e cambio passata provocano ricostruzione.
  Una sola cache serve tutte le passate: la simmetria puo annullare il riuso
  tra aggiornamenti consecutivi dell'impronta principale.
* Il grafo usa otto vicini interni e adiacenze topologiche CCG ai bordi.
  La distanza e una distanza di grafo, non la geodetica continua.
  Su una griglia quadrata piana regolare, la metrica a otto direzioni puo
  sovrastimare la distanza euclidea fino a circa l'8.24%; raffinare la stessa
  griglia senza aumentare le direzioni non elimina questo bias.
* I frame seguono il singolo predecessore con percorso piu corto.
  Percorsi di costo uguale non hanno tie-break esplicito; non c'e media tra
  fronti concorrenti o correzione globale della distorsione.
* La rotazione normal-to-normal e la formula di Rodrigues per la rotazione
  minima, seguita da proiezione e normalizzazione della tangente. Non emerge
  un errore algebrico evidente. Dot delle normali <=-0.25 interrompe il
  trasporto; l'assenza del frame puo propagarsi ai discendenti del percorso.
* Il passo UV usa la media delle proiezioni dell'edge nei due frame. Non
  corregge la differenza tra corda e arco o la lunghezza UV complessiva.
* Le cuciture hanno indici duplicati trattati separatamente. Un edge nullo
  collega i duplicati, ma non impone un'identita canonica dei loro frame:
  arrivarci da percorsi diversi puo lasciare differenze UV. Va misurato.
* Undo ripristina la geometria prima della preview Anchored. La cache
  memorizza puntatore/raggio/origine/asse, non una revisione della geometria.
  Non lo considero da solo un bug: per l'impronta occorre usare la stessa
  superficie di riferimento ripristinata. Validare la stabilita delle normali
  e della revisione quando si aggiungera un solver o un riuso piu aggressivo.

La diagnostica V4.1 lascia tutti questi comportamenti invariati per misurarli.

## Confronto delle direzioni

| Soluzione | Beneficio | Costo / limite per questo progetto |
|---|---|---|
| Discrete exponential map | Coordinate locali e adattamento diretto alla pipeline Dijkstra esistente | Mantiene errori del grafo e distorsioni su grandi protrusioni; da sola e vicina alla V4 |
| Fast marching su triangoli locali | Distanze meno dipendenti dalle direzioni della griglia | Non fornisce da solo orientamento e coordinate alfa 2D; richiede trasporto/log map aggiuntivi |
| Vector Heat | Trasporto e log map con sistemi sparsi prefattorizzati | Estrazione dominio, Laplaciano, fattorizzazione e invalidazione su patch deformabile |
| Affine Heat, variante locale | Mappa locale regolare, migliore stima integrata di rotazione e traslazione | Solver sparso e qualita triangolazione; candidato successivo su patch ridotta, non intera CCG |
| Parametrizzazione con correzione metrica locale | Riduce bias da predecessore e distribuisce la distorsione | Iterazioni limitate, protezione contro triangoli UV invertiti, compromesso tra angoli e area |

Fonti primarie consultate:

* Schmidt, Grimm, Wyvill, Discrete Exponential Maps, demo e codice originale:
  https://www.dgp.toronto.edu/~rms/software/expmapdemo.html
* Sharp, Soliman, Crane, Vector Heat Method:
  https://www.cs.cmu.edu/~kmcrane/Projects/VectorHeatMethod/index.html
* Soliman, Sharp, Affine Heat Method (2025):
  https://www.yousufsoliman.com/projects/the-affine-heat-method.html
* Implementazioni e strategie log map Geometry Central:
  https://geometry-central.net/surface/algorithms/vector_heat_method/

Le fonti dimostrano i metodi; la scelta sotto e una proposta di integrazione,
non una misura di prestazioni Blender derivata da questi lavori.

## Scelta: patch locale adattiva + exponential map + correzione metrica

Per V5 manterrei Dijkstra/Transport come inizializzazione e fallback.
La novita sostanziale sarebbe una correzione 2D su dominio ridotto e stabile,
indipendente dal numero totale di vertici e con cuciture unificate per topologia.

1. Costruire una patch locale sulla superficie di riferimento della preview.
   Usare PBVH per selezionare candidati e CCG per la connessione; nessuna
   saldatura basata sulla vicinanza euclidea, che unirebbe pieghe o lembi opposti.
2. Rendere canonici localmente i duplicati CCG. Posizioni/normali/frame del
   riferimento restano in cache sparse per questa impronta; nessuna modifica
   ai dati Multires, Sculpt Layers o alla logica Undo.
3. Ridurre la densita del dominio di calcolo in modo adattivo. Non usare
   indiscriminatamente un livello basso: raffinare dove normali, curvatura,
   errore geometrico o variazione UV indicano una piega significativa.
   I livelli Multires reali non vengono cambiati.
4. Inizializzare UV con il trasporto esistente su questa patch; creare
   triangoli locali coerenti attraverso le cuciture. Fissare origine e
   orientamento del brush; includere l'offset del punto di aggancio reale,
   evitando di trattare il vertice piu vicino come centro esatto.
5. Minimizzare un'energia metrica locale, ad esempio stress sulle lunghezze
   degli edge piu regolarizzazione rispetto alla mappa iniziale. Usare
   iterazioni limitate e line search che rifiuti nuovi triangoli UV invertiti.
   La sola media degli UV non basta: tende a restringere la mappa e cancellare
   la metrica invece di preservare la forma dell'alfa.
6. Interpolare UV ai campioni high-res del brush usando celle della patch,
   mantenendo separati i fogli topologici e controllando l'errore a campioni.
   Se il dominio adattivo non rappresenta una piega entro il budget, fallback
   esplicito e registrato; mai presentare il risultato come equivalente esatto.
7. Congelare dominio e UV durante ridimensionamento entro la copertura
   gia calcolata. Il raggio scala il campionamento. Per rotazioni usare una
   base neutra e rotazione UV; estendere il dominio quando serve senza
   ricalcolare in base a rumore dei campioni tra preview.

Budget iniziale da misurare, NON prestazione promessa: 2.000-8.000 nodi
di controllo locali; 3-5 passi di correzione e limite di tempo. Il budget
non deve troncare silenziosamente l'impronta. La valutazione high-res resta
proporzionale ai campioni toccati dal brush, come gia accade.

Prima correzione funzionale candidata, separata dalla diagnostica:
se Transport supera il cap, provare V3 con cap 300.000, mantenendo il fallback
V1/V2 finale. Questo conserva la protezione topologica quando possibile,
ma puo aggiungere il costo di due ricerche: non risolve da solo la qualita UV.

## Criterio di successo

Su una alfa asimmetrica, confrontare naso/guancia/orecchio con raggio e intensita
uguali: lettere meno stirate, continuita sulle cuciture e stessa orientazione
durante resize. Confrontare tempo di costruzione, riusi e fallback separatamente
dal tempo di deformazione. Se miglioriamo soltanto distanza radiale o un'alfa
gia simmetrica, non abbiamo dimostrato un miglioramento della proiezione 2D.

Stato: V5 progettata, non implementata e non compilata in questo pacchetto.
