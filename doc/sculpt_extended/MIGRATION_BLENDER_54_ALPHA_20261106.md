# Blender Sculpt Extended — migrazione sperimentale a Blender 5.4 Alpha

**Decisione:** 6 novembre 2026  
**Branch di lavoro:** `migration/blender-5.4-alpha`  
**Stato:** branch preparatorio creato da `main` del fork 5.2.3. **NON contiene ancora i sorgenti Blender 5.4**, né patch di migrazione, build o risultati di test.  
**Regola:** preservare `main` e la release stabile 5.2.3.

> **CORREZIONE CRITICA (2026-11-06):** questo branch è nato da `main`, che diverge dalla release 4 novembre; il compare GitHub mostra **207 commit release-only e 40 main-only**. Per non perdere le ottimizzazioni già distribuite la baseline autorevole del porting è il **tag `v5.2.3-sculpt-extended-20261104` (SHA `09a16d4b8482200101cb30728881ca4524edf019`)**. Creato il branch source baseline [**`migration/blender-5.4-alpha-release-baseline`**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/migration/blender-5.4-alpha-release-baseline). Questo documento rimane un piano; non assumere che i sorgenti 5.2.3 di questo branch contengano tutte le ottimizzazioni della release.
>
> **VINCOLO PRESTAZIONALE:** mantenere i miglioramenti già raggiunti in Commit locale, sparse layers, Multires Smooth, Undo, adaptive stroke, PBVH/redraw, gestione cache/budget RAM, Polypaint HR e Pose/Transform. [Leggere il nuovo inventario e i benchmark di preservazione](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha-release-baseline/doc/sculpt_extended/MIGRATION_54_PERFORMANCE_PRESERVATION_20261106.md).

## Scopo
Valutare un porting completo, a checkpoint verificabili, delle nostre funzioni Sculpt Extended dalla base 5.2.3 a un **commit specifico** di Blender 5.4 Alpha, evitando due migrazioni complete consecutive (5.3 e poi 5.4). Se incompatibilità o instabilità bloccano la migrazione, considerare 5.3 stabile come fallback.

## Perché provare 5.4
- Nuovo **3D Texture Paint** sperimentale (per futuro Surface Draw).
- Architettura di disegno PBVH già aggiornata con **Reduce Sculpt Buffers**; sviluppo meshlet ancora WIP.
- Curve di pressione 5.3 per Hardness / Auto-Smooth / Spacing.
- Miglioramenti upstream di Sculpt, Multires, texture e gestione delle immagini.
- Il porting dell'intera build consente di riutilizzare il codice ufficiale, ma comporta revisioni profonde delle nostre integrazioni con Multires/Undo/PBVH.

## Fissare i checkpoint prima di scrivere codice
1. Registrare **SHA esatto del tag release 4 novembre** (sorgente autoritativo per feature/velocità), SHA di `main` e divergenze da riconciliare, e **SHA/tag esatto dell'upstream 5.4 alpha**; non inseguire continuamente `main` Blender.
2. Salvare un elenco dei file modificati dal fork vs baseline 5.2.3 originale (includendo file aggiunti, DNA/RNA, versioning, CMake e test).
3. Compilare prima un **Blender 5.4 ufficiale senza modifiche**, con toolchain Windows adeguata, su build dir distinto.
4. Preparare una **nuova base 5.4** isolata (il branch attuale è soltanto la scaffolding 5.2.3 e non deve essere scambiato per il porting già eseguito).
5. Non cambiare tag/release/artefatti stabili; non aprire PR su `main` finché non sono soddisfatti i criteri minimi.

## Bug bloccanti e priorità emerse il 8 novembre 2026

**P0 — [#12 Symmetry Combine OFF / additive Strength](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/12):** non promuovere M2/M3/M6 prima di aver verificato che la disattivazione della doppia simmetria non lasci attiva involontariamente una diversa interpretazione degli assi e non generi sovrapposizione delle impronte / incremento Strength. Il sorgente attuale mantiene `sculpt_symmetry_reference_use_for_strokes` anche dopo `combine_local=false`; le impronte non-combined usano ancora il frame personalizzato. Test Feather ON/OFF e flag nativi X/Y/Z, anche con Sculpt Layer Multires.

**P1 — [#13 Gizmo shortcut/snap/anchor](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/13):** G/R/S devono selezionare il gizmo corrispondente, aggiornare immediatamente l'interfaccia e funzionare nel contesto manipolazione pertinente, senza sottrarre gli shortcut standard quando si scolpisce. Rendere coerenti backend nativo, vecchio operator Python, trasformazioni temporanee del 3D Cursor, snap e ancoraggio a vertice; validare Esc/Conferma, Snap On/Off, CTRL e modalità Edit/Sculpt.

Questi sono **requisiti prioritari di parità/regressione**, non funzionalità future opzionali. Nessun bug è ancora stato riprodotto in build e nessuna patch è stata applicata. Portare i fix anche nella release 5.2.3 soltanto dopo test separati.

## Ordine di migrazione (checkpoint separati)
**M0**: audit delle modifiche **tra tag release e `main`**, inventario delle ottimizzazioni attive e flag (vedere il Performance Preservation Contract), **baseline benchmark Windows del pacchetto release 4 novembre**, più compilazione base 5.4 ufficiale.  
**M1**: aggiunta definizioni dati proprie (DNA, RNA, kernel, CMake, blend I/O) e compilazione iniziale; salvare/rileggere un file con metadati di layer.  
**M2**: Sculpt Layers Multires: add/delete/select, visibility, strength, commit, Bake Visible→Base (preservare hidden), cambio livello L0↔Ln.  
**M3**: integrazione Undo/Redo con layer, first stroke, cambio modalità, save/reopen; proteggere dalla regressione degli spikes.  
**M4**: Polypaint HR: colore master al livello massimo, display L0, Brush Paint/Blur/Smear, attributi colore, UV bake/UDIM.  
**M5**: Face Sets HR: display, hide/isolate, gestures, maschere e deformazione.  
**M6**: Pose/Transform, pivot condivisi, simmetria, trasporto dei dettagli fra livelli, micro-dettaglio e strumenti speciali.  
**M7**: ottimizzazioni PBVH/nuovo draw: confrontare cache del fork con il layout 5.4, portare o sostituire solo dopo A/B; non sovrapporre automaticamente `MULTIRES_GEOMETRY_BUFFER_V01` e `CompactMeshNodeData` al nuovo draw system. **Le ottimizzazioni di stroke, commit, Undo, Smooth e RAM sono requisiti trasversali M1–M8**, non un'attività rinviata alla sola M7.  
**M8**: adattare la UI e le nuove funzioni sperimentali 5.4 (3D Texture Paint); evitare che il percorso Texture Paint interferisca con Sculpt Layers/Polypaint.  
**M9**: benchmark, QA completo, compilazione Windows release e packaging; Linux solo dopo parità Windows.

## File/aree particolarmente delicate
- `source/blender/blenkernel/intern/multires.cc` e `BKE_multires_sculpt_layers.hh` / `multires_sculpt_layers.cc`: compatibilità e persistenza dei dati HR.
- `source/blender/editors/sculpt_paint/mesh/sculpt.cc` e `sculpt_undo.cc`: stroke, layer-aware displacement, Edit Mode e confini Undo.
- `source/blender/draw/intern/draw_pbvh.cc`: nuove cache per-oggetto vs cache per-nodo custom, Polypaint HR e Face Sets HR.
- `source/blender/makesdna/`, `source/blender/makesrna/`, `source/blender/blenloader/`: layout, versioning e lettura dei vecchi .blend.
- `source/blender/editors/sculpt_paint/paint_stroke.cc`: Adaptive Stroke custom vs curve 5.3 e nuovi pennelli.
- `source/blender/editors/sculpt_paint/mesh/paint_image_3d.cc` upstream: nuovo Texture Paint, ancora sperimentale.

## Contratto prestazionale (obbligatorio ad ogni milestone)
- Fare riferimento al file [MIGRATION_54_PERFORMANCE_PRESERVATION_20261106.md](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/migration/blender-5.4-alpha-release-baseline/doc/sculpt_extended/MIGRATION_54_PERFORMANCE_PRESERVATION_20261106.md) del branch baseline.
- Confrontare **la release Windows 4 novembre**, non soltanto il branch `main`, su Draw/Clay/Smooth/Pose, dab e p95, commit locale, passaggi L0/Ln, Undo/Redo, Polypaint HR, Strength/visibility, memoria/RSS/VRAM, rendering PBVH. Ripetere su scene dense con flag equiparati.
- Non reintrodurre percorsi precedentemente respinti (compattazione durante stroke, cache progressiva regressiva, ecc.) e non considerare le proposte del tracker come ottimizzazioni già abilitate.
- I benefici upstream non annullano il vincolo di parità delle performance: in caso di regressione mantenere un fallback custom o rinviare il singolo percorso.

## Test di sbarramento
- File `.blend` di backup prima del test; **nessun round-trip su Blender standard** con garanzia di dati custom.
- Costruire un fixture con layer multipli, layer nascosto, modifica a L0, micro-dettaglio massimo, Polypaint HR/UDIM e Face Sets; salvare, riaprire, comparare stati.
- Stroke a diversi livelli e Undo/Redo, Edit Mode→Sculpt, Pose/Transform→ritorno a livello massimo senza spikes.
- Bake Visible→Base conserva layer nascosti; clone, subdiv e cambio livello non distruggono dati.
- Display colori/Face Sets/maschere e aggiornamenti locali in viewport.
- Memoria e responsività con mesh crescenti (~100k, ~1M, ~10M, ~60M; stress superiore solo se fattibile).
- Confronto A/B tra build 5.2.3 stabile e 5.4 sperimentale a parità di scene/driver/hardware.
- Annotare SHA sorgente e binario/test per ogni checkpoint (esito `PASS/FAIL`, dettagli e regressioni).

## Rischi e criteri decisionali
**Alta criticità:** DNA/.blend, Multires corner/grid identity, layer displacements L0/Ln, Undo e trasporto dettagli.  
**Media-alta criticità:** PBVH draw con nuova organizzazione buffer; color attributes Polypaint/Face Sets.  
**Media criticità:** pennelli e UI.  

La migrazione prosegue solo dopo compilazione del checkpoint e verifica dei test critici. Se il porting di M2/M3 richiede rifacimenti non sostenibili, mantenere la build 5.2.3 e rivalutare 5.3 stabile; non rinunciare ai file storici e non fare merge parziali nel ramo stabile.

## Riferimenti
- Release attuale: https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261104
- Memo PBVH: https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/main/doc/sculpt_extended/PBVH_DRAW_54_MEMO_20261106.md
- 5.4 upstream: https://projects.blender.org/blender/blender
- 3D Texture Paint: https://projects.blender.org/blender/blender/issues/163544
- PBVH meshlets: https://projects.blender.org/blender/blender/pulls/166181

**IMPORTANTE:** un branch Git creato da `main` non equivale a una base Blender 5.4. Il porting del codice ufficiale e i relativi test sono attività ancora da eseguire.
