# Sculpt Extended — Bmax, CurioMesh, SculptForge e remeshing: proposte implementabili
Data: 2026-11-12 | Stato: RESEARCH / DESIGN, sorgenti esaminati staticamente; niente merge, build o benchmark.
Base: v5.2.3-sculpt-extended-20261104. Vedi anche [MULTIRES_LAYERS_VNEXT_RESEARCH_20261112.md](MULTIRES_LAYERS_VNEXT_RESEARCH_20261112.md).
**Scelte confermate dall'utente:** amplificazione dell'ultima pennellata; Symmetry Blend; acquisire idee e ottimizzazioni utili dal remesher. Non introdurre dipendenze obbligatorie e non sovrascrivere funzioni già presenti.

## 1. Inventario dei riferimenti reali
| Fonte | Codice/documenti verificati | Idea esportabile | Limite |
| --- | --- | --- | --- |
| Bmax (GPLv3, fork Blender 5.2.3) | source/blender/editors/sculpt_paint/mesh/sculpt.cc, sculpt_undo.cc, sculpt_symmetrize_blend.cc; doc/bmax/manual/sculpt.md; doc/bmax/voxel_remesh_performance.md | net stroke delta, partial mirror pairing, blended masks/color, ottimizzazioni OpenVDB e preflight | i test del fork non sono test del nostro; pubblicazione source e Gumroad binari non coincidono necessariamente |
| CurioMesh GPLv3 | region.py, triadq/patch.py, triadq/checks.py, docs/ALGORITHM.md, multires.py | patch locale a bordo fisso, verifica topologica/quad, risultato su copia, candidato con rollback, recovery Multires | patch all-quad non garantita su regioni non a disco e contorni ambigui; non prova auto loops anatomici |
| CGAL Polygon Mesh Processing | documentazione isotropic_remeshing, constrained edge/vertex, projection | formalizzare quali edge/vertex NON possono cambiare e i vincoli di transizione | la funzione isotropic è principalmente triangolare, non fornisce direttamente il nostro quad remesher |
| Instant Meshes | repo wjakob/instant-meshes | separazione orientation field / position field con guide pennellabili | campi direzionali non garantiscono patch locale saldata alla mesh protetta |
| SculptForge | guide performance, geometry, subdivision/micro-levels | profiler per stroke, mask-preserving remesh su una regione, dettaglio che segue il livello inferiore | rappresentazione dati diversa da Blender CCG, strumenti non importabili 1:1 |

Riferimenti:
- https://github.com/MaxPuliero/Bmax/blob/main/source/blender/editors/sculpt_paint/mesh/sculpt.cc
- https://github.com/MaxPuliero/Bmax/blob/main/source/blender/editors/sculpt_paint/mesh/sculpt_symmetrize_blend.cc
- https://github.com/MaxPuliero/Bmax/blob/main/doc/bmax/voxel_remesh_performance.md
- https://github.com/CurioCrafter/CurioMesh/blob/main/triadq/patch.py
- https://github.com/CurioCrafter/CurioMesh/blob/main/docs/ALGORITHM.md
- https://doc.cgal.org/6.0/Polygon_mesh_processing/index.html
- https://github.com/wjakob/instant-meshes
- https://sculptforge.app/guide/geometry.html

## 2. Funzione A — Amplify / Repeat Last Stroke
**Bmax SOURCE-CONFIRMED:** SCULPT_OT_repeat_last_stroke implementato in sculpt.cc attorno alle linee 6369–6517; capture_stroke_delta usa Undo, il dato di deformazione sopravvive ai cambi brush/view ma non a invalidazione di identità della mesh. Ripetizione somma il **delta netto della pennellata precedente** (non ricampiona pressione, alpha, simmetria e maschera): 2x, 3x, 4x, non duplicazione esponenziale.

**Progetto Sculpt Extended V1**
- Capture alla chiusura di uno stroke geometrico validato, dalla *differenza finale realmente pubblicata* sul layer attivo. Non registrare la serie di dab; conservare solo l'ultimo delta e footprint su grid/sample. Il nostro journal esatto è il candidato di partenza. Nessuna cattura quando non esiste layer attivo valido o quando non è una modifica geometrica.
- Delta registrato in dominio persistente identificato da mesh/level/topology/revision/layer ID + fingerprint shape/frame, con costo proporzionale ai dirty samples. Rifiutare il replay dopo topology edit, cambio livello, bake/merge, nuova pennellata, cambio layer incompatibile o modifica della base che invalidi il frame; comunicare il motivo, non applicare silenziosamente dati stale.
- Operazione Amplify: aggiungere fattore scalare f al delta originale, con totale al primo replay pari a 1+f dell'effetto iniziale. f=1 replica Bmax; f=0.5 e f=-0.5 sono una possibile estensione non distruttiva da testare. Mai applicare f al risultato precedente o diventerebbe esponenziale.
- Una sola transazione Undo per comando; disegnare la superficie live; usare LayerGridTransaction e il normale trasporto tangent/object quando necessario. Registrare l'effetto sul layer originario (V1); NO duplicazione su layer correntemente diverso. Niente replay di movimento armature o filtro maschera come se fosse Sculpt.
- In V1 limitarsi a mesh fixed-topology/Multires layer attivo a livello corrente. Dyntopo richiede mapping dei nati e recovery Undo separati; non promuovere prima dei test.
- Interfaccia: Sculpt > Repeat / Amplify Last Stroke; comando Repeat senza UI (f=1), modal/popup Factor con ultimo delta conservato solo nella sessione. Shortcut non deve entrare in conflitto con keymap 5.4.

**Alternative e rischio**:
A) registrare differenza finale di coordinate nel CCG e ricostruire i delta tangent space a richiesta — semplice ma richiede validazione frame;
B) registrare differenza esatta nel payload persistente del layer — V1 preferibile quando Commit riesce e snapshot non costa full-grid;
C) registrare l'intero input brush e rifare il pennello — scartare per V1: risultati dipendono da maschere, pressione, spacing, alpha e overlap e divergerebbero facilmente.

**Gates**: Draw/Clay/Smooth/Grab/Elastic (uno alla volta), MultiRes L0/Ln, exact +/- f, stessa geometria con repeats, no drift, same active layer, hidden layers invariati, mask modified after stroke -> reject/semantica esplicita, visibility off, Undo/Redo 10x, perf footprint sparse.

## 3. Funzione B — Symmetrize Blend con maschere e Polypaint
**Bmax SOURCE-CONFIRMED:** sculpt_symmetrize_blend.cc usa mappa base mirror con seed spaziali, topology mirror fallback e propagazione del face/corner mapping reciproco. Le corrispondenze ambigue/non presenti non sono modificate. Mobility per vertice = (1 - clamp(mask,0,1)) per sample non nascosto; può fare media delle coordinate riflettendo l'altro lato, interpolare con Strength e combinare colore RGBA nell'attributo attivo. Su Multires elabora la geometria al massimo livello con operazione globale; su Bmax altri layer restano preservati e la correzione va nell'editable layer quando compatibile.

**Progetto per il fork**
- Riutilizzare la nostra mappa topologica, il movable symmetry reference frame e gli operatori di Symmetrize shape/topology esistenti. **NON** riscrivere un altro engine KD-tree. Distinguere: (a) one-shot BLEND, (b) Copy Shape, (c) Copy Topology, (d) Poseable Symmetry per-stroke.
- Matching reciproco v ↔ v' e face-corner mapping, con espliciti Self Pairs sul piano; su mesh posed distinguere coordinate rest/reference da pose corrente (non fare average world-space fra braccia in pose differenti).
- Per coppia valida: p_a e mirror(p_b), calcolare target medio con peso mobility di ciascun lato; maschera=1 => lato protetto esatto, la controparte si adatta; maschera soft => interpolazione continua. Usare uno Strength globale 0..1 e un profilo falloff opzionale solo in una seconda iterazione. Prima V1 sul reference plane per mesh non posata.
- Compatibilità Multires: campioni base + corner + coordinate CCG reciproche, seam ownership, maschere a livello corrente ma calcolo finale al master; correzione soltanto nel layer attivo, hidden/altre layer intatte. Se corrispondenza non sicura, no-op locale con report count; non fare nearest-surface indiscriminato.
- Colore: opt-in Blend Active Color; RGB lineare (non sRGB diretto), alpha definito, Float/Byte Point/Corner; più attributi non toccati. Undo geometry+color come una sola azione utente. Prima implementare geometry, poi color.
- Performance V1: full-surface solo nei casi piccoli/supportati; per large sculpts proporre ROI da Face Set/mask e coppie di grid, precompute dei frame una sola volta per coppia; conservare percorso globale come fallback trasparente. Profilare RAM/staging. Non ignorare i campioni condivisi sul perimetro.

**Gates**: X/Y/Z/custom plane, Strength 0/.25/1, mask 0/.25/.5/.75/1 e un lato interamente protetto, seam, mesh con metà senza corrispondenza, mirrored face topology non geometricamente simmetrica, color corners, layer hidden, L0 vs Ln, precisione, save/reopen/Undo, stress max. In V2 Pose-aware only after #6.

## 4. Funzione C — Remesh/Retopology locale e sicuro
Il bisogno dichiarato per #14: mask=1 protegge **esattamente** topologia/coordinate/attributi nella regione interamente protetta; mask=0 consente il remesh a densità massima scelta; 0<m<1 modula densità e intensità. Non confondere:
- Voxel Remesh volumetrico (può fondere volumi);
- Surface Remesh (split/collapse/flip/smooth sulla superficie);
- Quad Retopology / patch all-quad (trasforma il flusso di edge);
- Transfer Detail da vecchia a nuova topologia (un problema successivo).

### C1. Ottimizzazioni da Bmax (indipendenti dal risultato)
Dai benchmark documentati nel suo sorgente:
- skip del percorso BMesh di hole-search quando il boundary test su Mesh risulta chiuso;
- meno copie/array OpenVDB e conversioni duplicate; output packing parallelo;
- Fix Poles opzionale e failure guard per parte diventata vuota; possibile split "By Loose Parts".
- Sul loro fixture dense-closed ~1.31M triangoli: mediana ~968→217 ms, 4.46x; su fine-output ~996→992 ms, nessun vantaggio concreto. **Non trasferire il numero alla nostra build**.
- PRIMA confrontare con upstream 5.4: potrebbe avere già un percorso simile. Implementare come patch isolata, senza cambiare output in una fixture deterministica. Misurare anche RAM peak, attribuiti, FaceSets, UV, normals, e tempo dopo PBVH rebuild.

### C2. Remesh protetto/graduale — architecture contract
1. Snapshot immutabile (topologia, coordinate, normals, attributes, mask, face sets, layer identities e Multires master).
2. Produrre dominio protetto/free/transition; costruire **curve di confine vincolate**, individuare manifold e contorni multipli. I vertici e gli spigoli sul contorno protetto sono frozen; niente T-junction.
3. Definire edge-length field h(m, curvature, camera/scale) monotono: più piccolo nel libero, più grande verso il protected margin; nella zona mask=1 non eseguire alcuna operazione. Distinguere modulazione densità da interpolazione del movimento. No promessa di preservare m=1 se gli edge di frontiera sono troppo lunghi per il target: allargare la fascia nella regione libera o rifiutare in modo esplicito.
4. Passare al remesher con split/collapse/flip/smoothing *solo* nella regione non protetta, parametrizzando constraints ed eventuale relax tangenziale. Target non raggiungibile => report conservativo, nessun publish.
5. Il caso quad: usare patch della regione con bordo fisso e quad quality; se la topologia di confine impedisce un patch all-quad valido, non inventare quads degeneri. Opzioni: dividi in componenti a disco, estendi ROI soltanto nel free, o conserva triangoli nell'interfaccia dichiarandoli. La scelta "strict all-quad" deve rifiutare il terzo caso.
6. Validazione prerelease: finite/positive area, no flipped face/duplicate/nonmanifold, valenza/fans, componenti, boundary loops, Euler characteristic quando preservation richiesto, no self-intersections locali rilevabili, UV/material/paint/facesets corretti, protected exact match; misura errore simmetrico BVH verso source con RMS/p95/max e controllo zone sottili.
7. Publish come singolo Undo dopo preflight; in caso fail rollback e reason. Nessuna perdita silenziosa dei dati.

**CGAL come riferimento tecnico, non dipendenza decisa:** isotropic_remeshing supporta regione, edge_is_constrained_map e vertex_is_constrained_map, protect_constraints, proiezione e quality. È un remesher per triangoli; patch quad e trasferimento Master CCG restano problemi nostri. https://doc.cgal.org/6.0/Polygon_mesh_processing/index.html

**CurioMesh per la patch:** region.py seleziona Face Set/selection/Vertex Group/cage, prepara boundary, calcola un patch, mostra preview e scrive dopo; triadq/patch.py affronta parity, suddivisione in dischi, fixed seam, Coons grid e untangle. docs/ALGORITHM.md usa candidate scoring e quality gates, con rollback quando projection non passa; i test topologici locali non sono prova globale d'intersezione.
https://github.com/CurioCrafter/CurioMesh/blob/main/region.py
https://github.com/CurioCrafter/CurioMesh/blob/main/triadq/patch.py
https://github.com/CurioCrafter/CurioMesh/blob/main/triadq/checks.py

### C3. Rischio Sculpt Layers e Multires
Un remesh che cambia numero/connettività della base **invalida le identità corner/grid**. La V1 non deve provare a modificare direttamente l'oggetto che conserva Master Sculpt Layers; deve produrre una nuova mesh, preservare lo sculpt sorgente, e richiedere conferma per trasferire shape/UV/color/FaceSets/weights. Per i layer:
A) bake il risultato visibile su nuova base + creazione nuovo Multires con reshape dalla sorgente: possibile V1 con *perdita esplicitata dell'editabilità individuale*;
B) trasferire per-layer displacement con BVH nearest, rays, barycentric face/corner e nuovi frame, poi validare contro ricomposizione originale: V2 sperimentale, costo e drift da misurare;
C) se l'operazione non cambia davvero topologia (solo shape), tenere grid identity e usare la strada Multires attuale.
Nessun silent transfer o claim di conservazione layer con retopology arbitraria.

## 5. Nuove idee che possiamo sviluppare da queste tecniche
**Quality-First Mesh Validator** condiviso tra Remesh #14, Symmetrize #15, QuadriFlow+ e Photogrammetry: risultati e threshold in formato strutturato, es. ACCEPTED / ROLLED_BACK + reason. Con contatori e overlay locali di problemi, non solo un messaggio finale.

**Guided Quad Patches**: combinare CurioMesh fixed seam con campi orientati dipinti tipo Instant Meshes. V1 guide come edge hints o curve su Facet Set, non generatore completo di loop anatomici; la ricostruzione resta limitata a patch semplici e singola regione. https://github.com/wjakob/instant-meshes

**Seam-Aware Mask Constraint API** condivisa, ma algoritmi distinti: mappatura mask a hard/soft constraints, boundary IDs, freeze esatto m=1, transition band scalabile, attribute interpolation. Usata da remesh superficiale e Symmetrize Copy Topology, senza confondere i due solvers.

**Preview / Reject / Commit**: costruire i candidati in oggetto proxy o buffer isolato, visualizzare differenza percentuale di silhouette/superficie, rifiutare prima della pubblicazione. CurioMesh distingue misure SOURCE_EXACT/PROXY_EXACT/SAMPLED; adottare lo stesso principio di *livello di prova* per le scene troppo dense.

**Remesh e dettaglio**: usare una valutazione del master HR per la proiezione o per Multires Reshape sul nuovo cage soltanto dopo la validazione. CurioMesh multires.py usa operatori Blender nativi per ricostruire i livelli ma non preserva gli Sculpt Layers originali come layer indipendenti. https://github.com/CurioCrafter/CurioMesh/blob/main/multires.py

**Gradual Stroke Gain (idea nuova)**: dopo Amplify, provare un controllo non distruttivo di moltiplicazione dell'ultimo stroke su un transient last-stroke buffer che può essere regolato più volte *senza creare nuovi accumuli di errore*. Commit finale nativo. Deve usare il delta originale e un solo source reference, come Strength Preview.

## 6. Matrice rischi / priorità
| Feature | Riuso presente nel fork | Rischio | Ordine |
| --- | --- | --- | --- |
| Amplify Last Stroke (mesh + Multires, fixed topology) | Dirty sample/Undo/layer transaction | Medio | Dopo messa in sicurezza transazioni |
| Symmetrize Blend geometry | Mappa topologica, frame, maschere, layer/commit | Medio-alto | Dopo Amplify, separato da Posed |
| Symmetrize Blend Color | Polypaint HR/Color attributes | Alto finché Undo/Polypaint fragile | Dopo G1 P0 |
| Voxel Remesh fast path | OpenVDB Blender nativo | Basso-medio, ma possibile conflitto 5.4 | Dopo confronto upstream |
| Protected Surface Remesh | masked_remesh.py sperimentale + Gizmo | Alto (topologia e attribuiti) | Branch sperimentale |
| Patch all-quad fixed seam | CurioMesh, QuadriFlow già presente | Alto su topologie complesse | PoC regioni a disco |
| Layer-preserving retopology | Sculpt master+BVH projection | Molto alto | Solo dopo V1 geometry/quality |
| Posed Symmetry | #6 | Alto | Priorità funzionale separata; condividere map/constraints |

## 7. Protocollo operativo per Codex
- **M0:** fissare SHA/tag, confrontare il codice effettivamente presente; annotare versioni Bmax/CurioMesh/CGAL e policy GPL/attribuzione. Eseguire test nativi del fork prima di ogni patch.
- **M1:** refactor minimo di shared LastStrokeDelta e LayerGridTransaction (se il secondo ha superato i test), implementare Repeat con f=1; test invarianti mesh/layer/Undo.
- **M2:** Symmetry Blend geometry, input map reciproca e non matching; test mask e frame. Solo dopo abilitarne color.
- **M3:** Voxel fast path boundary check e copy reduction, test output equivalence; patch soltanto se la 5.4 non offre già vantaggi equivalenti.
- **M4:** Mask Constraint API + Validatore geometrico; fallimento transazionale + local patch con bordo protetto; alla prima iterazione supportare casi semplici documentati, no obbligo di quads perfetti su ogni topologia.
- **M5:** quad guides e trasferimento detail sperimentale su nuova mesh; nessun merge in stable senza test di conservazione.

Fixture obbligatorie: cube/Suzanne/creature with eyelids, pose asimmetrica, shell aperte, 1/2/3 loops di bordo, patch non a disco, UV seam, mask m=0/.25/.5/.75/1, Face Sets, layer hidden, attributi Byte/Float, Polypaint, nonmanifold, boundary freeze, 100k/1M/10M/60M campioni, stress solo con budget RAM. Test di fallimento e rollback veri; hash dei dati protetti; negative path no mutation.

### Go/No-go
**ACCEPT** solo con no corruption, no spikes/cracks/unintended mesh welding, Undo/Redo e save/reopen corretti, mask=1 immutata esattamente, qualità non peggiore del riferimento, p50/p95 e picchi RAM senza regressioni sistematiche. Ogni PR con linee/file sorgente, SHA, fixture, release flags, test realmente eseguiti e scelta KEEP / REVISE / ROLLBACK.

## 8. Link incrociati
- [#14 Mask-Adaptive Protected Remesh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/14)
- [#15 Gizmo Symmetrize Topology/Mask](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/15)
- [#6 Poseable Symmetry + Quick Rig](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/6)
- [Master Roadmap Codex](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/feature/pre54-symmetry-remesh-fixes-v01/doc/sculpt_extended/MASTER_ROADMAP_CODEX_20261108.md)
- [Performance tracker precedente](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/v5.2.3-sculpt-extended-20261104/doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md)

**Nota:** trattare codice di terzi come riferimento per audit e porting legale GPL con attribuzione, non copiare cieco patch molto grandi incompatibili con il fork o 5.4. Benchmarks, pubblicazione dei binari e ipotesi qui sono distinti.
