# NanoMesh Sculpt Layers — architettura proposta
_Data: 2026-11-06 · Stato: DESIGN / NON IMPLEMENTATO_

## Obiettivo e requisiti concordati

Progettare un sistema **nativo, interno a Blender Sculpt Extended** per distribuire, manipolare e incorporare geometria ripetuta (scaglie, squame, spine, placche, escrescenze, pori e inserti) sulla superficie Multires. Deve supportare l'editing diretto in Sculpt Mode con pennello, ma continuare a funzionare **fuori Sculpt Mode**, in Object Mode, durante l'animazione e il rendering.

**Idea chiave:** il NanoMesh Layer conserva dati procedurali originali (mesh sorgente + distribuzione + ancoraggi) mentre produce output derivati **Instances**, **Vector Displacement** o **Hybrid**. Non distruggere la sorgente quando si genera o si effettua il bake del displacement. Le correzioni scolpite devono stare in Sculpt Layers distinti.

### Decisioni di prodotto

1. **NanoMesh è un tipo logico di layer**, non una funzione temporanea del pennello. UI integrata nella tab Multires/Sculpt Layers, ma **storage separato** dai delta nelle griglie degli Sculpt Layers. Non duplicare i sistemi già presenti.
2. **NanoMesh Brush in Sculpt Mode** che riusa la selezione già disponibile: Face Sets/Face Sets HR e Mask/Mask HR. Operazioni Paint/Add, Erase, Density, Orient, Scale, Relax, Replace/Randomize, con simmetria e pressione ove supportate.
3. **Mask e Face Sets sono input della distribuzione**, non l'archivio persistente delle istanze. La mask può controllare probabilità/densità (con inversione); un Face Set o un insieme di ID limita l'area; i dati delle istanze sono persistenti anche cancellando successivamente la mask.
4. **Binding Live e Snapshot.** Live segue modifiche di Mask/Face Sets, Snapshot congela la distribuzione o la selezione. Implementare una policy stabile per il seed/random: una piccola modifica a una mask non deve riposizionare casualmente tutte le istanze.
5. **Operazione inversa:** NanoMesh → Mask, Face Sets, density/footprint field, Polypaint e influenza su displacement; ad esempio rigonfiare la pelle alla base di una scaglia e incidere automaticamente il bordo.
6. **Tre modalità di output** per lo stesso master:
   - **Instances:** geometria vera, istanziata, modificabile e non distruttiva; può avere sottosquadri, bordi liberi e spessori.
   - **Vector Displacement:** campo vettoriale derivato dalle istanze e valutato in griglie Multires (oppure esportabile come mappe); può essere baked in un layer geometrico scolpibile.
   - **Hybrid:** istanze per silhouette/sovrapposizioni/spessori + displacement reale di integrazione/raccordo sulla pelle. Deve funzionare anche fuori Sculpt Mode.
7. **Due operazioni diverse:** Generate (campo displacement live, parametrico e ricalcolabile) vs **Bake to Sculpt Layer** (delta persistenti e modificabili con i pennelli, con la sorgente NanoMesh preservata).
8. **Le correzioni manuali** vivono in uno Sculpt Layer di correzione separato. Se cambia drasticamente la distribuzione, non eliminarle silenziosamente: reproject/rebind, avviso sui dati orfani oppure congelamento del risultato precedente.
9. **Deformazioni:** l'istanza resta agganciata alla superficie anche cambiando livello Multires, deformando il corpo con Pose/Grab, Shape Keys o Armature nei percorsi supportati. Differenziare Rigid (trasla/ruota), Conform (segue curvatura locale), Elastic (segue strain/stretch; necessita ancoraggi multipli).
10. **Compatibilità con Adaptive LOD/MicroDetail:** master precisione indipendente dal livello visibile, cache di rappresentazione adatte allo schermo, nessuna distruzione dei campioni ad alta risoluzione quando si lavora ai livelli bassi. Il sistema non deve obbligare a subdividere tutta la mesh.
11. **Output per produzione:** geometria realizzata, campo Vector Displacement, scalar displacement dove sufficiente, normal map, UV/UDIM dove implementabile, export mesh. Il bake non deve essere dichiarato senza perdita dove la topologia della scaglia non è rappresentabile.
12. **Stabilità:** preservare Undo/Redo, save/reopen, dati degli altri layer, seam e corrispondenze CCG; evitare regressioni spikes o rebuild globali a ogni dab.

## Architettura proposta

```text
Mesh base + Multires + Sculpt Layers
       │                 ▲
       ├── Surface Evaluation / Layer Snapshot (ordine definito, no loop)
       │         │
       │         ▼
       └── NanoMesh Layer (master procedurale persistente)
              ├─ Source geometry / source asset
              ├─ Distribution records: stable IDs, seed, density, brush edits
              ├─ Region selectors: Mask / Face Sets (Live or Snapshot)
              ├─ Parametric anchors: base face/corner ID + local (u,v)
              ├─ Instance transform + orientation + variants
              └─ Projection/deformation parameters
                         │
                   Output evaluation
                    /      |       \
               Instances  VDM      Hybrid
                    \      |       /
                     Viewport / Object / Render
                                  │
                         Optional explicit Bake
                                  ▼
                     Separate Multires Sculpt Layer
                                  +
                      Sculpt corrections layer
```

### Ancoraggi robusti

- Non salvare soltanto coordinate XYZ o un indice di vertice CCG del livello corrente. Salvare identità topologica/parametrica stabile su superficie base: face/corner ID, coordinate locali (u,v), frame tangente e orientamento; valutare la posizione sulla **superficie composta prevista**.
- In presenza di modifiche topologiche, usare le identity/remap primitives esistenti (conservatively); se la corrispondenza non è valida, marcare l'ancoraggio come orphan e richiedere rebind. Non fingere un remap corretto.
- **Binding alla superficie:** definire esplicitamente quale stadio dello stack si campiona (base + layer selezionati o superficie visibile prima del NanoMesh). Evitare che l'output VDM alteri la superficie sulla quale le sue stesse istanze si ancorano; niente dipendenze circolari.
- Le modifiche su Sculpt Layer a livello basso spostano gli ancoraggi sulla superficie valutata, senza alterare il master di distribuzione. Il trasporto del dettaglio protetto rimane un gate importante, non un problema già risolto universalmente.
- Per scaglie rigide basta una trasformazione locale; Conform ed Elastic possono richiedere campionamenti multipli della superficie e strain tensor locale.

### Conversione in displacement

1. Determinare footprint locale, orientamento e surface frame degli elementi.
2. Proiettare la geometria soltanto nelle regioni rappresentabili come **un singolo valore vettoriale per punto della superficie**; fornire controlli su direzione di projection, distanza massima, normal/tangent mode, occlusioni e profondità.
3. Risolvere collisioni/intersezioni/overlap: non sommare ingenuamente i delta delle scaglie. Serve una policy (highest/closest/priority/smooth union) con robusti limiti e fallbacks.
4. Scrivere il campo derivato su buffer/regione per-corner CCG o su microtile, senza sovrascrivere i payload master degli Sculpt Layers esistenti.
5. In modalità **Hybrid**, lasciare come istanze le porzioni con spessore, superfici multiple o sottosquadri, generando invece il raccordo alla pelle tramite Vector Displacement; evitare doppio rendering dello stesso rilievo.
6. **Bake**: convertire il campo a delta Multires come operazione esplicita, con salvataggio del master e Undo. Le correzioni manuali restano separate.
7. Preferire tangent-space displacement per deformazioni organiche; prevedere opzione object-space dove utile, con test severi di trasporto/frame/seams. *Vector displacement non può creare automaticamente topologia extra.*
8. L'export in texture richiede UV/UDIM e definizione degli spazi vettoriali; il baking diretto in griglie CCG non deve richiedere UV.

### Valutazione fuori Sculpt Mode

Il pennello è **un editor** dei dati persistenti. La valutazione delle istanze + deformazione derivata deve stare in un componente nativo dell'oggetto/modifier/depsgraph, valutabile in Object Mode, in animazione e nel render. Un modificatore NanoMesh nativo è una possibilità architetturale da validare, **non** una scelta di codice già attuata. Considerare anche integrazione con Geometry Nodes solo come backend/rapid prototype.

**Ordine dei modificatori** da specificare/testare: pre/post Armature, pre/post Multires, Shape Key, cache della superficie di ancoraggio e render displacement. Un modifier tradizionale con istanze aggiunte dopo Multires non scrive automaticamente nei CD_MDISPS: l'accoppiamento con lo Sculpt Layer deve essere esplicito.

### Dati e performance

- Instance master: asset mesh condiviso, record compatti per trasformazione/anchor, varianti, stable ID, seed e stato di edit; istanze non realizzate finché non serve.
- Cache regionali: dirty-area/dirty-generation, dependency halo, BVH per proiezione, batch GPU/viewport e aggiornamenti locali. Evitare rebuilding globale del CCG/PBVH e Undo memfile per piccoli brush stroke.
- Policy LOD: il master resta a precisione piena; viste a livello basso mostrano istanze o proxy anche quando il CCG coarse non può descriverle. Il campo VDM viene campionato solo se la risoluzione è sufficiente. Non usare soglie fisse di livello 2/4/6 per tutte le mesh.
- Undo: diff per eventi su distribution records e regioni VDM modificate, save/reopen con ownership/versioning e validazione dei dati.
- Displacement di correzione: cache derivata da master + layers di correzione, con invalidazione riproducibile; la rigenerazione preserva le modifiche tramite coordinate comuni, remap controllato o congelamento dello stato.

## Componenti GIA' PRESENTI e da riusare (non reimplementare)

- Multires Sculpt Layers, strength/visibility/reorder/group/merge/commit e bake visible-to-base.
- Face Sets HR, HR Masks, Polypaint HR, Draw Transport e Detail Transport Copy (con limitazioni/feature flags).
- API in `source/blender/blenkernel/BKE_multires_sculpt_layers.hh`: `detail_delta_import_to_new_layer()`, `bake_texture_displacement_to_active()`, `bake_shrinkwrap_to_active()`, corner/vertex IDs e remap dopo Edit Mode; nessuna di queste è già un operatore NanoMesh.
- Geometry Nodes per instancing/distribuzione, brush workflow e surface projection già sviluppati nel fork, con supporto variabile.
- Adaptive Sculpt LOD è **soltanto specificato**, MicroDetail tile compatti è **proposto** e Poseable Symmetry live per-stroke è **incompleta**: non citarli come funzionalità già presenti nella release.

## Rischi non risolti

1. Ancoraggi ad alta stabilità dopo cambiamenti della topologia o del frame tangente (seams, mirrored UV o corner flip).
2. Impatto di Pose/Grab sui layer protetti: conservazione **del contenuto** non equivale a conservazione **visiva** delle rughe.
3. Proiezione di scaglie sovrapposte e sottosquadri su un solo campo di displacement: conversione inevitabilmente lossy in molti casi.
4. Dipendenze cicliche fra superficie valutata, NanoMesh output e scultura di correzione.
5. Undo/VRAM/RAM su scene con 50k+ istanze e Multires estremi; gestione efficace di densità, area dirty e object lifecycle.
6. Divergenza fra rendering displacement/viewport e geometria bake; definire autorità del master e validazione.
7. Face Sets HR disponibili solo in certe configurazioni; fallbacks e conversioni non distruttive.

## Piano incrementale e test/gate

| Fase | Scopo | Test minimo di accettazione |
|---|---|---|
| A | Backend prototipo di distribuzione, Face Sets/Mask, seed e istanze | 10k–50k scaglie su creature, salvataggio/riapertura, Live/Snapshot |
| B | NanoMesh Brush Paint/Erase/Density/Orient/Scale/Relax | Strokes Undo/Redo; maschere e Face Sets; nessun drift casuale dei master |
| C | Ancoraggio parametrico al Multires + deformazioni | Pose/Grab a livello basso e ritorno al massimo; nessun salto o spike, ID conservati |
| D | Output VDM live regionale | Risultato su CCG/regioni senza toccare gli altri layer; fallback per non-proiettabili |
| E | Hybrid geometry + skin connection | Scaglie sovrapposte con bordi reali + pelle raccordata senza intersezioni visibili |
| F | Bake to separate Sculpt Layer e correzioni | Re-edit di distribution + correzioni preservate/reprojected o segnalate |
| G | Evaluation in Object Mode / rig / render | Animazione Armature e rendering consistenti con viewport; no depsgraph cycles |
| H | MicroDetail/Adaptive LOD e export | Memoria/latency misurate, master invariato a livelli bassi, mappe e mesh coerenti |

**Test regressione obbligatori:** mesh con scaglie su braccio che ruota, Face Sets HR + maschera, cambio livelli L0→Lmax→L0, Undo/Redo, save/reopen, hidden layers, forme sovrapposte, seam e bordi UV, Material/Geometry Nodes, rendering e deformazione con Armature. Confrontare prototipo ON/OFF con build stabile (4 novembre 2026).

## Priorità e dipendenze

- NanoMesh/Mask/Face Sets Brush + istanze è **P1**, ma l'integrazione **VDM/Hybrid** richiede una fase separata più difficile.
- Mantenere **P0 Poseable Symmetry e MicroDetail** e **P1 Adaptive LOD** già concordati. Non accorpare questa feature ai loro branch finché la stabilità non è verificata.
- Implementare in branch sperimentale opt-in. Nessuna regressione sugli spikes/Undo e nessuna modifica anticipata del formato DNA prima dei test di fattibilità.

## Link e riferimenti

- [Audit ZBrush / SculptForge / Blender](COMPARATIVE_AUDIT_SCULPTFORGE_ZBRUSH_FORUMS_20261104.md)
- [Audit caratteristiche già presenti](ADDITIONAL_EXISTING_FEATURES_AUDIT_20261104.md)
- [Roadmap Pose/MicroDetail/LOD](ROADMAP_POSE_MICRODETAIL_LOD_20261103.md)
- [Adaptive LOD project](ADAPTIVE_SCULPT_LOD_PROJECT_20261101.md)
- [Codice API Multires](../../source/blender/blenkernel/BKE_multires_sculpt_layers.hh)
- [Riepilogo tutte le funzioni ZBrush e portabilità](ZBRUSH_PORTABILITY_FEATURE_BACKLOG_20261106.md)

_Nota legale: gli strumenti ZBrush sono riferimenti comportamentali; nessuna copia di codice/asset proprietari._
