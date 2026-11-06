# Memo tecnico — Blender 5.4 PBVH Draw / Reduce Sculpt Buffers
**Data analisi:** 6 novembre 2026  
**Progetto:** [Brunoarmeni86/Blender-Sculpt-Extended](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended)  
**Destinazione:** `main` (solo documentazione; nessun intervento sulla build stabile)  
**Stato:** valutazione progettuale, **non** implementato / compilato / benchmarkato nel fork.

## Decisione concordata
1. **Priorità A:** analizzare e provare l'integrazione mirata di **Draw: Reduce Sculpt Buffers** di Blender 5.4.
2. **Priorità B, in seguito:** valutare il nuovo sistema PBVH/meshlet e culling GPU **quando sarà maturo**.
3. Evitare l'aggiornamento integrale del fork 5.2.3 a 5.4 nell'immediato e **non sostituire** direttamente `draw_pbvh.cc` senza portare le personalizzazioni.
4. Eseguire prove in un **branch/build sperimentale separato**, senza alterare la release stabile.

## Stato upstream — snapshot al 2026-11-06
- [Design: PBVH drawing (Blender 5.4), #160654](https://projects.blender.org/blender/blender/issues/160654): piano di riorganizzazione del drawing Sculpt.
- **Integrato:** commit [`63250e3a1f` — Draw: Reduce Sculpt Buffers](https://projects.blender.org/blender/blender/commit/63250e3a1f) (15 ottobre 2026). Cambia la disposizione dei buffer: da buffer per nodo PBVH a VBO per attributo a livello di oggetto e indici/intervalli per le regioni. **Non** introduce ancora Multi-Draw Indirect (MDI) né il culling meshlet completo.
- **Integrate modifiche preparatorie** al disegno/draw abstraction in 5.4: approfondire le dipendenze prima del backport; evitare cherry-pick ciechi.
- **Ancora aperta:** [PR #164606 — Meshlet based culling](https://projects.blender.org/blender/blender/pulls/164606); infrastruttura culling in revisione.
- **WIP (aperta 5 novembre 2026):** [PR #166181 — Sculpt meshlets](https://projects.blender.org/blender/blender/pulls/166181); non considerare il sistema PBVH meshlet una feature completa della 5.4 Alpha.
- La snapshot è valida alla data indicata; ricontrollare la situazione upstream prima dell'integrazione.

## Situazione nel nostro fork
La base pubblicata più recente è **Blender Sculpt Extended 5.2.3**, release 4 novembre 2026 (vedere `README.md` e release). La nostra implementazione PBVH **non è quella standard di Blender 5.2**.

**File da confrontare:**
- `source/blender/draw/intern/draw_pbvh.cc`
- `source/blender/blenkernel/BKE_paint_bvh.hh`
- `source/blender/editors/sculpt_paint/mesh/sculpt.cc`
- Strutture GPU/draw e gestione degli attributi coinvolti nel commit upstream.

**Adattamenti già presenti nel fork** (esaminati nei sorgenti della linea Sculpt Extended):
- `DrawCacheImpl`, `AttributeData`, VBO per nodo e maschere `dirty_nodes`.
- `MULTIRES_GEOMETRY_BUFFER_V01` / `BLENDER_MULTIRES_GEOMETRY_BUFFER`: esperimento opt-in di buffer geometrici Multires.
- `CompactMeshNodeData`: percorso sperimentale di disegno mesh compatto.
- Supporti personalizzati per **Polypaint HR**, attributi attivi/render, selezione e layout dei VBO.
- Supporti di visualizzazione **Face Sets HR**, incluse diverse modalità e invalidazioni localizzate.
- Cache e notifiche di aggiornamento per posizione, topologia, visibilità, maschere, attributi.

**ATTENZIONE:** audit puntuale e test devono essere effettuati sulla **precisa SHA di `main` scelta per il branch sperimentale**, non assumendo identità con i precedenti checkpoint `release/sculpt-extended-native-20261020`.

## Valutazione di integrabilità
| Componente 5.4 | Integrabilità preliminare | Decisione |
| --- | --- | --- |
| Fix locali PBVH / GPU draw | Relativamente alta; controllare dipendenze | Candidati a backport selettivo |
| **Reduce Sculpt Buffers** | **Media**, rischio architetturale medio-alto | **Prima prova da realizzare** |
| Nuova astrazione `draw::Batch` | Più invasiva sulla base 5.2.3 | Integrare solo se richiesta dalle dipendenze |
| Meshlet culling generale #164606 | Bassa ora, PR aperta | Monitorare |
| PBVH Sculpt meshlets #166181 | Bassa ora, WIP | Rimandare |

La migrazione della cache da layout **per-nodo** a layout **per-oggetto** interferisce potenzialmente con le nostre cache e percorsi Multires. **Non sostituire il file intero**: reintrodurre/riadattare esplicitamente gli aggiornamenti per Polypaint HR, Face Sets HR e layer, preservando i fallback.

## Benefici plausibili e limiti
**Potenzialmente migliorabili:** aggiornamenti localizzati dei buffer, memoria GPU dei buffer duplicati, traffico CPU→GPU, costo di redraw dopo stroke, navigazione su mesh dense.

**Non direttamente migliorati:** suddivisione Multires, calcolo Sculpt Layers/Commit, tempi di Undo, trasporto del dettaglio, algoritmo del pennello.

Il futuro culling meshlet può ridurre geometria rasterizzata e draw call; **non equivale automaticamente** a riduzione proporzionale di VRAM: geometria scartata dal disegno può restare residente.

Nessuna percentuale di accelerazione è attualmente verificata sulla nostra build.

## Piano operativo
1. **Audit dei diff upstream:** confrontare `63250e3a1f` con il nostro `main` a SHA fissata; enumerare API GPU/draw modificate, commit prerequisite, conflitti e variazioni al layout cache.
2. **Baseline:** registrare tempi/ram/VRAM su build Windows stabile 5.2.3 e Blender 5.4 Alpha ufficiale, stessa scena e driver.
3. **Branch sperimentale dedicato** dal `main` congelato: portare la gestione buffer minima con feature flag, mantenendo intatto il percorso tradizionale. Se il refactor richiede troppe dipendenze, valutare un prototipo isolato o una base 5.4 separata.
4. **Parità funzionale:** validare `DrawCacheImpl` per mesh e Multires grids; Polypaint HR su L0 e livelli superiori; Face Sets HR; maschere; hidden faces; attributi colore attivi/render; materiali; toggle wireframe; modifica topologia; Undo/Redo; livello Multires; riapertura file.
5. **Benchmark A/B/C:** A = release attuale; B = fork con nuovi buffer; C = Blender 5.4 Alpha. Scene: media densità, ~60M, poi stress test ~170M se la memoria lo consente. Misurare frame time mediano/p95, freeze al primo draw, update dopo stroke, tempo di caricamento/ricostruzione buffer, CPU RAM, GPU VRAM, eventuali crash.
6. **Decisione:** mantenere il backport soltanto se misurabilmente vantaggioso e senza regressioni visive/funzionali; altrimenti restare sulla soluzione corrente e riprovare quando meshlets e nuovo rendering PBVH saranno stabili.

## Criteri minimi di accettazione
- **Zero regressioni** visibili in Polypaint HR, Face Sets HR, maschere e rendering Multires L0/Ln.
- Nessun aumento incontrollato di RAM/VRAM o rallentamento marcato del primo stroke/draw.
- Undo/Redo, save/reopen e cambi di modalità/livello invariati.
- Nessun cambiamento dei dati Sculpt Layers / Polypaint durante il solo draw.
- Guadagni riproducibili rispetto alla stessa scena/configurazione; riportare risultati positivi e negativi.

## Fonti e collegamenti
- [Blender PBVH drawing design #160654](https://projects.blender.org/blender/blender/issues/160654)
- [Upstream Reduce Sculpt Buffers commit 63250e3a1f](https://projects.blender.org/blender/blender/commit/63250e3a1f)
- [Meshlet based culling PR #164606](https://projects.blender.org/blender/blender/pulls/164606)
- [Sculpt meshlets PR #166181](https://projects.blender.org/blender/blender/pulls/166181)
- [Sorgenti draw_pbvh.cc del fork, ramo main](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/main/source/blender/draw/intern/draw_pbvh.cc)
- [Release stabile 5.2.3 del 4 novembre](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/v5.2.3-sculpt-extended-20261104)

**Nota:** questo memo registra la scelta progettuale. Non afferma che le patch upstream siano già trasferite né che esistano benchmark del fork con il nuovo PBVH.
