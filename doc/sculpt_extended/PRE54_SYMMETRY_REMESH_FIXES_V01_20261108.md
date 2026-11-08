# Handoff per Codex — correzioni Sculpt Extended prima della migrazione 5.4
**Data:** 2026-11-08  
**Branch di laboratorio:** `feature/pre54-symmetry-remesh-fixes-v01` (originato dalla release `v5.2.3-sculpt-extended-20261104`, non da `main`).  
**Decisione aggiornata dell'utente:** **non è necessario terminare subito le correzioni**. Questa branch deve fungere da **banco di prova e archivio di proposte / prototipi di codice per Codex**. **Nessun merge sulla build stabile** finché non saranno dimostrati correttezza e vantaggi prestazionali.

## Indice operativo
- [#12 Additive Strength e transizione doppia simmetria](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/12) — **P0**: impedire impronte duplicate, reinterpretazioni degli assi e riattivazioni implicite.
- [#13 Gizmo G/R/S, interfaccia, snap, anchor](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/13) — **P1**: coordinate di gizmo, scorciatoie contestuali, aggancio e rebind.
- [#14 Remesh protetto adattivo alla maschera](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/14) — **P1**: `mask=1` congelamento esatto, `mask=0` massima risoluzione, 0<m<1 gradazione continua.
- [#15 Symmetrize del gizmo: copiare topologia e raccordare maschera](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/15) — **P0/P1**: geometria speculare con connettività sorgente dove possibile, transition solver dove necessario.

## Stato delle modifiche nel branch: prototipi NON validati
| Percorso | Modifica già proposta in codice | Cosa **non** dimostra |
| --- | --- | --- |
| `scripts/startup/sculpt_extended_integrated/masked_remesh.py` | Criterio `all(mask >= 1-eps)` per le facce congelate; campo `edge_target(mask)`; controlli `sculpt_remesh_mask_coarsening` e `sculpt_remesh_mask_falloff` | Nessuna prova di manifold, assenza di autointersezioni, qualità del collar, propagazione corretta degli attributi o prestazioni su milioni di poligoni. Rimane un **surface remesh BMesh**, NON un voxel remesh volumetrico. |
| `symmetry_shape_v2.py` | Semi basati su **edge manifold verificati sul piano** oltre alla ricerca nearest; propagazione topologica esistente | Non supporta ancora tutti i casi di seam complesso, topologia differente, due bordi disgiunti e superfici speculari parzialmente corrispondenti. |
| `symmetry_topology_mask.py` | Congela solo facce completamente mascherate; in presenza di valori parziali utilizza il trasferimento di **forma**; ritorna il metodo usato | Il trasferimento di forma **non equivale a copiare la topologia**. Il raccordo protetto su topologie diverse usa ancora il vecchio fallback. |
| `symmetry_reference_preview_transition.py` | Se il gizmo è mostrato, Symmetrize suggerisce il piano del riferimento; feedback sulla modalità; toggle Combine OFF preferisce simmetria nativa quando questa è abilitata; evita alcuni re-enable impliciti | Non prova ancora l'eliminazione delle doppie impronte o la correttezza di `Symmetry Feather`; un piano del gizmo mostrato non significa necessariamente che sia abilitato per gli stroke. |
| `reference_native_snap.py` | Aggiornamento preview su `MOUSEMOVE` oltre al timer più frequente | Non sostituisce l'uso del cursore 3D, non garantisce timing corretto degli eventi modali, non risolve ancora snap/anchor Multires. |
| `doc/sculpt_extended/tests/pre54_symmetry_remesh_regression.py` | Fixture Blender headless per seam posed e protezione mask | I test **non sono stati eseguiti**; fixture incompleta su sistemi complessi e su tool GUI. |

**IMPORTANTE:** tutte le patch di cui sopra sono candidati WIP che Codex deve poter riscrivere, sostituire o scartare. Nessuna è stata compilata, misurata o approvata come correzione di produzione.

## Architettura proposta: un solo motore condiviso di vincoli della maschera
Il remesh generico (#14) e Symmetrize topologico del gizmo (#15) hanno vincoli in comune, ma **non sono la stessa operazione**. Proporre un modulo riutilizzabile `mask_remesh_constraints` (nome indicativo) che fornisca:
1. Valori `m` continui in [0,1]; interpolazione robusta su edge/facce e gestione dei punti nuovi.
2. Contratto **hard** su ogni faccia completamente mascherata: coordinate, connettività, indici logici e attributi non mutati; bordo condiviso preservato (nessuna T-junction).
3. Fascia di transizione parametrizzabile attorno alla regione congelata, con falloff continuo **dell'intensità** e target edge-length locale graduato; separare i parametri che controllano densità da quelli che controllano deformazione.
4. Vincoli locali split/collapse/flip/triangolazione e controllo qualità.
5. Transazione di publish/Undo: produrre un candidato in memoria, validarlo e applicarlo **solo se tutti i vincoli passano**.

### Copia topologica: ordine degli algoritmi da valutare
1. **Topologia equivalente:** ottenere una mappa intera fonte→destinazione per propagazione da edge/face seed sul piano del gizmo; non dipendere da coordinate speculari vicine per superfici posed.
2. **Topologia differente:** conservare il lato sorgente e la zona destinazione totalmente protetta; ricostruire soltanto dominio libero + collar con stitching *constrained*; preferire componenti con contorni e ariste esplicite.
3. **Multires:** l'operatore attuale `symmetry_multires.py` è una copia di **dettaglio/forma senza cambiare topologia**. Lasciarlo separato dalla ricostruzione topologica, a meno di progettare una conversione esplicita che conservi livelli e Sculpt Layers.
4. **Sicurezza:** nessuna pubblicazione se comparsa di self intersections, facce invertite, buchi, bordi nonmanifold, perdita UV/Face Sets/materiali/Color Attributes, movimento di punti totalmente protetti; fallimento descrittivo e rollback.

### Nota tecnica sui limiti del vecchio fallback
- `symmetry_topology_mask.py`: se esistono facce completamente protette, il path preferisce `symmetry_shape_v2.transfer` per conservare la topologia; quando fallisce va a `symmetry_transition_build.assemble` (solo per messaggi riconosciuti). È fragile usare **stringhe di eccezione** per scegliere algoritmi: preferire errori strutturati con codici.
- `symmetry_transition_uv.py`: protezione con `any(mask > 1e-6)` e parametrizzazione armonica in un disco; **non supporta in generale** più componenti, fori, zone di maschera separate, input non manifold.
- `symmetry_surface.py`: `clipped_half` rifiuta topologie non a disco, più contorni; la corrispondenza UV dipende fortemente dalla scelta del bordo.
- `symmetry_transition_build.py`: verifica parziale manifold/overlap e conserva alcuni attributi; serve audit loop UV e IDs di fonte. Non attribuire al vecchio codice garanzie non dimostrate.
- `masked_remesh.py`: il codice precedente classificava faccia protetta con `any(v[mask] > 1e-6)`; la bozza nuova con `all(v[mask] >= 1-eps)` **non basta** da sola per assicurare collar perfetto o densità graduale di qualità.
- **Né shape transfer né remesh locale BMesh costituiscono un vero Voxel Remesh capace di fondere due volumi.** Questa è una milestone separata, non un presupposto di #14/#15.

## Ordine suggerito a Codex (senza obbligo di fare tutto immediatamente)
1. **Audit e fixture riproducibili:** analizzare diff tra tag release e branch, eseguire `python -m py_compile` per i Python toccati, poi lo script di regressione in una build Blender compatibile; aggiungere unit/geometry fixtures per `m=0, 0.25, 0.5, 0.75, 1`, seam off-center, piano ruotato, lato posed, mesh aperta e chiusa.
2. **P0 simmetria** (#12): definire comportamento della modalità Locale / Reference / Combined, tracciare matrici per pass e compensazione Strength, testare Feather ON/OFF, layer e Undo prima di adottare callback Python WIP.
3. **Topologia corrispondente** (#15): convalidare la propagazione da seam e gestione dell'orientamento; non cambiare coordinate sul lato sorgente; prevedere gestione per isole e bordi nonmanifold come errori non distruttivi.
4. **Remesh adattivo** (#14): misurare qualità e costo della versione BMesh, verificare che non compaiano triangoli degeneri e che densità e maschera si comportino come richiesto; disegnare modulo di vincoli condivisi.
5. **Constrained collar + cambio topologia** (#15): recuperare le parti affidabili di `symmetry_transition_uv.py`, ma introdurre vero dominio libero/protetto, interpolazione edge e corretto trasferimento attributi; gestire i contorni multipli solo se dimostrabile.
6. **Gizmo** (#13): unificare i due operatori, G/R/S contestuali, snap diretto e ancoraggio valido in Multires dove sicuro; confrontare la preview Python e il backend C++.
7. **Benchmark finale:** 100k / 1M / multi-milioni, cold/warm, p50/p95, RAM/VRAM, Undo, save/reopen; nessun percorso per-dab più lento significativamente rispetto alla 5.2.3 stabile.
8. **Solo dopo:** trasferire i commit validati al branch di migrazione 5.4; non fare merge in `main` senza approvazione e test.

## Test e comandi rapidi per Codex
```bash
git fetch --all --tags
git switch feature/pre54-symmetry-remesh-fixes-v01
git diff v5.2.3-sculpt-extended-20261104...HEAD -- scripts/startup/sculpt_extended_integrated
python -m py_compile scripts/startup/sculpt_extended_integrated/{masked_remesh,symmetry_shape_v2,symmetry_topology_mask,symmetry_reference_preview_transition,reference_native_snap}.py
# Da eseguire con Blender custom, dalla root del repository:
blender -b --factory-startup --python doc/sculpt_extended/tests/pre54_symmetry_remesh_regression.py
```
`py_compile` controlla soltanto la sintassi Python e **non** sostituisce Blender né verifica C++, DNA/RNA o integrità della mesh. In ambiente Windows adattare sintassi shell e path.

### Report richiesto ad ogni iterazione Codex
- SHA upstream e branch; file cambiati; funzione e ragione.
- Stato: **IDEA / PROTOTYPE / STATIC-VALIDATED / BLENDER-RUNTIME-VALIDATED / BENCHMARKED / STABLE-CANDIDATE**.
- Test eseguiti davvero (non simulati), PASS/FAIL e scene/flag/driver.
- Regressioni residue, impatto su Sculpt Layers/Undo/performance, prossimo passo.
- Esito di decisione `keep / revise / rollback`. **Vietato dichiarare corretta una patch solo perché compila.**

## Riferimenti di progetto
- [Branch pre54 WIP](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/feature/pre54-symmetry-remesh-fixes-v01)
- [Registro salvaguardia performance 5.4](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha-release-baseline/doc/sculpt_extended/MIGRATION_54_PERFORMANCE_PRESERVATION_20261106.md)
- [Piano migrazione 5.4](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha/doc/sculpt_extended/MIGRATION_BLENDER_54_ALPHA_20261106.md)
