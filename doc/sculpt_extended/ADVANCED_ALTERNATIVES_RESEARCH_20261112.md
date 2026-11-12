# Sculpt Extended — alternative architetturali e idee di ricerca approfondite
Data: 2026-11-12. Stato: ricerca di fonti e ipotesi; nessun prototipo o benchmark compilato da questo documento.
Complementi: [Multires Layers VNext](MULTIRES_LAYERS_VNEXT_RESEARCH_20261112.md), [Bmax/Curio/Remesh](BMAX_CURIO_REMESH_RESEARCH_20261112.md).
Scopo: non fermarsi al port di Bmax/CurioMesh ma valutare soluzioni alternative che riducano il lavoro complessivo sul lungo periodo; distinguere algoritmi maturi da proposte speculative.

## A. Tre modelli diversi per il displacement nei layer
**A0 — Attuale (fonte di verità):** per-corner Multires CCG master + layer sparse delta in frame tangente, usati dal Commit e dal playback. Conservare come baseline.

**A1 — Hierarchical residual tiles (PoC indipendente):** ogni layer può memorizzare base displacement a livello grossolano + residual ad alta frequenza in tile opzionali. Un edit grossolano cambia il componente coarse, mentre i residual rimangono immutati; un layer solo di piccoli pori occupa soltanto regioni non-zero. Potenzialmente utile per future MicroDetail e LOD. NON assumere che decomporre i delta esistenti sia lossless rispetto agli attuali frame/aggregazioni; il PoC deve dimostrare round trip numerico per seam, livelli bassi e normali.

**A2 — Sparse tile states ispirati a OpenVDB:** tre stati concettuali per blocco 2D (ZERO implicito / CONSTANT / DENSE). Le idee OpenVDB riguardano una struttura voxel 3D, quindi qui si trasferisce **la rappresentazione** e non la libreria/algoritmo in blocco. Inactive ZERO già presente: l'unico vero gap è constant tile e sparse sub-grid, con statistiche di occupancy. Prima valutare quanto costano gli header e lookup sulle nostre CCG e se le regioni sono realmente uniformi.
Fonte OpenVDB: https://www.openvdb.org/documentation/doxygen/overview.html

**A3 — Compressed cold block cache:** lasciare i campioni hot float3 non compressi; per block inattivi/Undo usare un codec lossless veloce tipo LZ4/Zstd; prefetch solo in thread propri quando consentito, mai scrivere in strutture Blender fuori dal main/authorized eval. Evitare compressione di tutto il payload a ogni stroke. La compressione lossy in stile ZFP (fixed accuracy) è possibile solo per cache rigenerabili o MicroDetail esplicitamente a precisione limitata, non per master/Undo esatti senza consenso; il paper ZFP fornisce error bounds ma non prova che errori di surfacing sotto frame change non causino spikes.
https://github.com/meta/zstd
https://www.openvdb.org/documentation/doxygen/faq.html
https://zfp.readthedocs.io/en/develop/faq.html

### Esperimento storage
Campionare 1000 tile da: layer 100% denso, layer cicatrice 5%, Polypaint HR, layer nascosto e layer Pose grossolano. Misurare zero/constant/mixed occupancy, entropia byte, MB mem prima/dopo, decode p95, costo di conversione e bitwise save/reopen. GO solo se comprensione dei pattern dimostra un vantaggio; no slot extra in DNA finché test e migrazione non passano.

## B. Trasporto dei dettagli sotto pose/rig: tre modi matematici
Data deformazione locale Phi(x) e Jacobiano J:
1. **Full Jacobian transport:** d' = J d. Mantiene la deformazione affine locale ma può allungare e comprimere i pori insieme al rig. Usarlo quando si vuole stretch fisico coerente.
2. **Rotation-only (polar decomposition):** J = R S, d' = R d, mantiene approssimativamente ampiezza del dettaglio mentre lo orienta. Potenzialmente preferibile per skin pores su articolazioni. S non va perso per la forma grossolana, solo per il micro-delta relativo.
3. **Tangent frame recalculation:** convertire il displacement in object space, trasportarlo sulla superficie deformata e ricodificare in un nuovo frame per-sample (approccio già presente nel Commit low-to-high); occorre test nei punti di alta curvatura/seam.

La formula per lo stroke mirrored su mesh armature posed, d_b ≈ J_b M J_a^{-1} d_a, è valida localmente **soltanto se J_a è ben condizionato**. Per normal-only displacement e forti torsioni conviene talvolta lavorare con frame locale e match topologico invece di inverting una matrice quasi singolare.

Scelta proposta: API DetailTransportStrategy = FULL_GRADIENT / ROTATION_ONLY / TANGENT_REPROJECT, default conservativo uguale al comportamento esistente dove disponibile. In V1 un solo tipo di deformation per test, nessuna sostituzione generale.
Fonti per limitazioni LBS vs Dual Quaternion e blend deformazione:
https://onlinelibrary.wiley.com/doi/10.1111/cgf.15062
https://skinning.org/

### Esperimento transport
Fixture cilindro pelle con 1 mm di incisioni, spirale su corno, articolazione stretta e scala non uniforme; rotazioni 45/90/160°, rig LBS e DQ; confronto lunghezza/energia del dettaglio, bending, min/max delta, deviation rispetto a reference HR, seam values, undo/redo e layers hidden.

## C. Render/Strength Preview: progressive refinement con budget temporale
**Fonte:** SculptForge usa profiler per Brush/Normals/BVH/GPU upload e un frame-budget governor per la visualizzazione su mesh grandi. Non assumere che questo migliori i calcoli di Commit.
https://sculptforge.app/guide/performance.html
https://sculptforge.app/performance/

Proposta indipendente da CCG mixed-level:
- durante una gesture slider, mantenere snapshot master esatto e preview view-dependent, eventualmente coarse; dare priorità ai tile visibili, vicino al cursore, silhouette e zone di modifica più grandi;
- budget UI per frame, **senza** aggiornare il payload persistente, scrivendo solo nel buffer preview o in proxy;
- alla conferma calcolo esatto su tutto il supporto e barrier prima di nuove operazioni che potrebbero leggere la superficie;
- indicatori preview approximate/actual, nessun export/bake da buffer non finale;
- fallback immediato a current path se region hash non valido o rendering non supportato.

Esperimento A/B/C: coalescing-only, proxy-level preview, ROI-culling preview, 100% layer denso. Registrare first visual response, max hitch, finalize latency, RAM/VRAM e differenza viewport finale. Se diventa difficile isolare la geometria UI dalle API di Sculpt, preferire coalescing-only e rimandare.

## D. Limit evaluation selettiva invece di globale
OpenSubdiv offre PatchTable, LimitStencilTable e valutazione dei punti di superficie a locazioni parametriche arbitrarie, inclusi subset di facce; backend CPU e GPU differenti.
https://github.com/PixarAnimationStudios/OpenSubdiv/blob/dev/documentation/tutorials.rst
https://opensubdiv.org/docs/osd_overview.html

Proposta di ricerca: usare stencil/topology-only derivative reuse per campioni delle sole griglie dirty nel Commit e nella conversione delta→frame, se producesse esattamente la stessa limit surface con crease/boundary/corner settings. Gli stencils possono restare validi per più modifiche del cage, ma frame tangenti e displacement possono comunque cambiare e invalidare cache. Non trasferire un PatchTable direttamente dentro un CCG senza contratto di equivalenza.
Test: quad, ngon, pole, crease, extraordinary vertices, UV-boundary, different Subdiv quality; comparare numeri su CPU prima di pensare alla GPU.

## E. Perché una GPU direct-delta non è automaticamente una buona idea
Possibilità V2: comporre Strength su buffer GPU per visualizzazione, usando compute shader e culling. Rischi concreti: il layer master e Undo sono ancora CPU-owned; il readback può eliminare il vantaggio; 100M samples richiedono grandi buffer aggiuntivi e il target ha una GPU con VRAM limitata. Disegnare più veloce NON velocizza il Commit CPU.
Blender 5.4 PBVH drawing design considera MDI e culling di meshlet, ma quest'ultimo è ancora WIP: non investire in un backend custom parallelo finché non è confrontato.
https://projects.blender.org/blender/blender/issues/160654
https://projects.blender.org/blender/blender/pulls/166181

PoC ammesso soltanto come viewport-preview sperimentale, mai come source of truth, con fallback GPU OFF.

## F. Remesh guidato da errore, non solo da densità desiderata
CurioMesh valuta candidates, quality hard gates e distanze geometriche simmetriche. Una strategia alternativa è **multi-candidate selection con budget esplicito**: per regione non protetta generare 2-3 candidati con seed/constraint differenti, confrontare p95/max deviation, quad Jacobian, poles e perf; scegliere solo candidates che preservano vincoli topologici. Il costo è sensato per operazioni offline/preview, non a ogni dab.
https://github.com/CurioCrafter/CurioMesh/blob/main/docs/ALGORITHM.md

**Error policy:** indicare se proof della topologia è SOURCE_EXACT, PROXY_EXACT oppure SAMPLED; non confondere percentuale di silhouette con garanzia matematica di nessuna intersezione. La nostra mesh può essere altamente densa, ma la validazione sul proxy deve dichiarare l'assenza di prova globale.

**Constrained remeshing:** CGAL formalizza vertex_is_constrained, edge_is_constrained, protect_constraints e proiezione dopo split/collapse/flip; attenzione quando la lunghezza dei bordi vincolati rende impossibile il target. Prima allargare la fascia libera o ritornare FAIL con motivo, non mutare la maschera=1.
https://doc.cgal.org/6.0/Polygon_mesh_processing/index.html

## G. Alternative da RESPINGERE finché non dimostrate
- Duplicare la mesh completa per ogni layer o ogni variazione Strength: RAM esplode su 122M campioni.
- Trattare cambi topologici come una semplice remap di indici corner: incompatibile con gli Sculpt Layers.
- Usare nearest-surface generico per rimediare a simmetria topologica ambigua: rischio trasferimento sul lato anatomico sbagliato.
- Saltare stitching sui bordi delle griglie per velocizzare hot-cache senza test di continuità.
- Un secondo sistema di Undo indipendente da Blender, che perde ordinamento con livelli, filtri, Shape Keys e Polypaint.
- Compressione con perdita del Master Layer senza tolerance contract esplicito; nelle deformazioni l'errore può amplificarsi.

## H. Decision matrix (da aggiornare dopo i benchmark)
| Idea | Potenziale | Rischio | Prototipo |
| --- | --- | --- | --- |
| Coalescing Strength eventi | Alta reattività | Basso | Subito dopo fix P0 |
| ColorMasterSwap typed Undo | Molto alto per freeze | Alto | Gate di correttezza |
| Atomic LayerGridTransaction | Alto affidabilità | Medio-alto | Fault injection prima |
| grid→PBVH index | Medio per layer sparsi | Basso-medio | Contatori e A/B |
| Rotation-only detail transport | Alto artistico | Medio-alto | Una fixture rigged |
| Constant/zero/dense tile states | Memoria potenziale | Alto | Offline dati prima |
| Limit stencil selettivo | CPU potenziale | Medio-alto | Test numeric parity |
| GPU Strength Preview | Vantaggio visivo possibile | Alto | Solo dopo 5.4 baseline |
| Multi-candidate remesh validated | Alta qualità | Alto costo offline | Region patch semplice |
| ZFP lossy master | RAM elevata ma errori | Molto alto | NO per default |

## I. Handoff finale
Prima di qualsiasi sviluppo: (1) misurare e riprodurre; (2) test di errore e rollback; (3) implementare un solo PoC; (4) parità geometrica rispetto al vecchio ramo; (5) A/B memory e p50/p95; (6) solo allora mantenere, revisionare o eliminare. Non affermare nessun risultato prima della build.

Relazioni: [#6 Poseable Symmetry](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/6), [#7 MicroDetail](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/7), [#8 LOD](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/8), [#14 Remesh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/14), [#15 Symmetrize](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/15).
