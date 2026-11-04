# Audit addizionale delle funzionalità già presenti — 4 novembre 2026

> Inventario del **codice**, non conferma che tutte le opzioni siano attive nella portable approvata.
> Esaminati il branch `checkpoint/adaptive-commit-foundation-20261102` (`bda4dcbf0e99`) e alcuni documenti di `checkpoint/stable-20261027` (`1eaaa7d9fe8b`).
> Non sono stati compilati nuovi binari né effettuati test grafici dal vivo.

## Funzionalità da NON ricreare

| Sistema | Evidenza di implementazione | Stato e limiti |
|---|---|---|
| HR Extract Face Set/Mask | `sculpt_multires_hr_extract_v05.hh`, UI `space_view3d_sculpt_extended.py` | Native operator, richiede Sculpt su CCG, fonte corretta, nessun pending geometry; V05 protetto da `BLENDER_MULTIRES_HR_EXTRACT_V05=1` o preset RC10; non presumere sempre attivo. |
| Detail Transfer / import nuovo Sculpt Layer | `scripts/startup/sculpt_detail_transport_copy_v01/__init__.py`, `sculpt_detail_transfer_v01.hh` | Preview della differenza geometrica + import nativo in un nuovo layer; sperimentale e soggetto a restrizioni oggetto/modificatori/Undo. Non equivale a un live deformation transport universale. |
| Texture displacement procedurale nativo | `DNA_modifier_types.h` (`sculpt_displacement_*`), `multires.cc` | Texture, mapping UV o alternativi, strength, midlevel; live displacement valutato via Multires. Non equivale a microtile/geometria sparsa a densità variabile. |
| Bake displacement/shrinkwrap→active layer | `BKE_multires_sculpt_layers.hh` e `sculpt_ops.cc` | Esiste percorso di bake verso layer, con limiti e mask support. |
| Polypaint HR UV bake | `sculpt_polypaint_uv_bake_v01.hh` | Bake di un **singolo tile** UV in un'immagine (res 64–4096); non chiamarlo pipeline UDIM automatica completa. Necessario nome UV corretto. |
| Multires HR Face Sets | `BKE_multires_sculpt_layers.hh`, `sculpt_face_set.cc` | Master HR lazy/persistente, mask e visibilità; alcuni tool base-only vengono bloccati anziché fingere comportamento HR; esistono percorsi opt-in. |
| High-res Sculpt mask persistence | `multires.cc`, `sculpt_undo.cc` | Griglie mask e percorso di flush/save/Undo già esistenti. |
| Delta Smooth attivo layer ai livelli inferiori | `sculpt_layer_delta_smooth_v01.hh` | Capture riferimento e sample stride dal layer master, fallback/ottimizzazioni coarse; limitato a Laplacian nei casi isolati e rifiuta live displacement/shrinkwrap, configurazioni incompatibili. |
| Draw Transport mesh / Multires | `sculpt_draw_transport_v01.hh`, `sculpt_draw_transport_mesh_v01.hh` | Trasporto/preview di dettaglio, rotazione/scala/strength; non generale per ogni brush e ogni combinazione di simmetria. |
| Shape Keys local update / fast path | `multires_sculpt_layers.cc` `shape_key_fast_update()` | Ottimizzazione condizionata a CCG valido, PBVH Grids, shape key relativa e assenza di stroke/filter o alcuni deformatori. Non dare per acquisito supporto Pose completo. |
| Elastic Grab Exact/Accelerated, VDM Draw | `brushes/elastic_deform.cc`, `brushes/draw_vector_displacement.cc` | Percorsi nativi presenti, opzioni Exact/Accelerated e cache; performance e coerenza layer da provare. |
| Named Polypaint attributes | `DNA_modifier_types.h` `MultiresColorAttribute`, `BKE_multires_sculpt_layers.hh` | Canali HR multipli e selezionabili: **non** un compositing stack con blend modes colore. |

## Gap REALI per ZBrush/SculptForge da non confondere con le funzioni sopra

1. **Morph Brush**: lo snapshot nativo `SCULPT_OT_multires_layer_snapshot` è esplicitamente una **copia nascosta additiva**, non un restore point né un brush per ripristinare/blendare localmente una forma. Vedere `sculpt_ops.cc`.
2. **Blend modes geometrici dei layer**: `MultiresSculptLayer` conserva name, strength, flags, level, grids, L0 offsets e metadati di gruppo, senza un campo persistente per il blend mode; la composizione corrente è additiva. Eventuali modalità nuove necessitano semantica, Undo, migrazione e benchmark prima di UI.
3. **Selection Sets nominati universali**: Face Sets, mask, hide e snapshot di geometria sono strumenti separati. Non è ancora dimostrato un sistema unico di snapshot nominati per maschera/visibilità/colore/forma. Può essere utile per Poseable Symmetry e LOD, ma è sotto le priorità concordate.
4. **Poseable Symmetry live per-stroke**: il trasferimento della forma per corrispondenza topologica già esiste, ma non risolve ogni dab della brush su arti già posati.
5. **MicroDetail**: griglie layer sparse **zero** già esistono, ma non micromesh tile compressi per dettaglio non nullo e viewport/brush residency.
6. **Adaptive LOD**: progetto già dettagliato dal 1 novembre; non ancora implementata la CCG a livelli misti.

## Rischio critico: storage isolation ≠ visual detail isolation

Come documentato in [doc/sculpt_extended/BRUSH_LAYER_ISOLATION_20261022.md](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/1eaaa7d9fe8b7514edff6979dabebefa835e5202/doc/sculpt_extended/BRUSH_LAYER_ISOLATION_20261022.md), il Commit può proteggere i payload degli altri layer **senza** preservarne necessariamente la forma visibile durante un brush che lavora sulla superficie composta. Un layer attivo può creare delta compensativi che appiattiscono il dettaglio di un layer protetto.

- Smooth/Delta Smooth: già esiste isolamento specifico con limiti noti.
- Draw/Clay/Plane ecc.: percorsi `Preserve Other Layers` presenti per configurazioni determinate.
- **Pose/Grab/Elastic/G-R-S**: preservare il dettaglio richiede trasportarlo insieme alla deformazione (tangent frame, layer contribution), non sottrarre un riferimento fisso. **Questa è una dipendenza diretta della Poseable Symmetry**.

Test discriminante: noise/rughe su layer nascosto o protetto, pose della regione sul layer attivo, verifica numerica che i delta protetti restino identici **e verifica visiva** che le rughe seguano la nuova superficie.

## Rischio prestazionale già misurato: Polypaint/Undo

Il [doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/doc/sculpt_extended/LAYER_PERFORMANCE_OPTIMIZATION_TRACKER_20261102.md) riporta un caso reale con:
- Commit geometrico locale ~395 ms (non il principale responsabile del blocco);
- `POLYPAINT_LAZY_MASTER` seguito da global rebuild CCG ~10.1 s;
- una sequenza Undo memfile Polypaint fino a ~58.3 s, con ulteriori ricostruzioni globali ~9.8–13.3 s.

Queste cifre valgono solo per la scena e la configurazione misurate, **non** per ogni utente. Prima di MicroDetail/LOD, evitare di moltiplicare snapshot memfile globali e rebuild dell'intera CCG nelle transazioni locali.

## Proposta di ordine, mantenendo le tre priorità dell'utente

1. **Poseable Symmetry:** partire da `symmetry_shape_v2.py` (mapping), C++ symmetry pass e Pose Brush; prima verificare visual transport di layer e Undo.
2. **MicroDetail:** riusare sparse zero-grid, data model CCG e procedural displacement, poi progettare microtile di dettaglio effettivo non nullo.
3. **Adaptive LOD:** riprendere il documento del 1 novembre e introdurre vere regioni a risoluzione differenziata.
4. **Solo dopo:** Morph Brush e blend modes, se richiesti; non confonderli con snapshot e Strength già presenti.

## Percorsi di codice del checkpoint di lavoro

- [source/blender/editors/sculpt_paint/mesh/sculpt_multires_hr_extract_v05.hh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_multires_hr_extract_v05.hh)
- [source/blender/editors/sculpt_paint/mesh/sculpt_polypaint_uv_bake_v01.hh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_polypaint_uv_bake_v01.hh)
- [scripts/startup/sculpt_detail_transport_copy_v01/__init__.py](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/scripts/startup/sculpt_detail_transport_copy_v01/__init__.py)
- [source/blender/editors/sculpt_paint/mesh/sculpt_layer_delta_smooth_v01.hh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_layer_delta_smooth_v01.hh)
- [source/blender/editors/sculpt_paint/mesh/sculpt_draw_transport_v01.hh](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_draw_transport_v01.hh)
- [source/blender/editors/sculpt_paint/mesh/sculpt_ops.cc](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_ops.cc)
- [source/blender/blenkernel/intern/multires_sculpt_layers.cc](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/blenkernel/intern/multires_sculpt_layers.cc)
- [source/blender/makesdna/DNA_modifier_types.h](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/makesdna/DNA_modifier_types.h)
- [source/blender/editors/sculpt_paint/mesh/sculpt_pose.cc](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/bda4dcbf0e99266b9d47fc1dbd0adbe66d19adc2/source/blender/editors/sculpt_paint/mesh/sculpt_pose.cc)
