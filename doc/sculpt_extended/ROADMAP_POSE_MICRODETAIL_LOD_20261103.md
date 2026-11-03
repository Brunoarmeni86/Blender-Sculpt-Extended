# Roadmap prioritaria — Poseable Symmetry, MicroDetail e Adaptive LOD
_Annotazioni concordate il 3 novembre 2026. Documento di progetto, non changelog di funzionalità già implementate._

## Decisione e ordine delle priorità

1. **P0 — Poseable Symmetry e Quick Pose** in Sculpt Mode: [#6](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/6).
2. **P0 — MicroDetail Multires** con storage compatto, preservazione dei dettagli e compatibilità con layer: [#7](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/7).
3. **P1 — Adaptive Sculpt LOD** regionale per viewport, valutazione e microgeometria: [#8](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/8).

Stabilizzazione dell'attuale Build (spikes, layer Multires, Preserve Other Layers, Undo/Redo) è una **condizione trasversale**: non deve essere sospesa, ma neppure bloccare i prototipi isolati dei tre sistemi. Audit preesistente: [#3](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/issues/3).

## 1. Poseable Symmetry / Quick Pose

- Simmetria topologica indipendente dal piano spaziale dopo deformazioni asimmetriche.
- Rest/reference pose e associazione persistente dei vertici speculari; validazione di topologia e punti senza partner.
- Quick Pose interno allo Sculpt Mode: pivot rapido, falloff, regioni via Face Sets/maschere, move/bend/rotate/twist, ritorno immediato allo sculpt.
- Compatibilità con Sculpt Layers, Multires, maschere, Preserve e Undo/Redo.
- Prima versione con topologia invariata; gestione delle modifiche topologiche e performance da affrontare separatamente.
- **Criterio di successo:** coppie di regioni in pose differenti ricevono strokes topologicamente speculari; geometria/layer protetti invariati; save/undo/reopen verificati.

## 2. MicroDetail

- Dati micro nelle griglie compatte/sparse, limitando le allocazioni e gli aggiornamenti ai tile sporchi.
- Dettagli che seguono deformazioni effettuate ai livelli sottostanti; nessuno spike, drift o seam.
- Sviluppo iniziale su spostamenti scalari; microvector/VDM da valutare in iterazione successiva.
- Editing e preview coordinati con Adaptive LOD; export/bake displacement e normal ove appropriato.
- Undo diff, persistenza nel .blend, layer e maschere coerenti.
- **Criterio di successo:** modificare volumi a livello inferiore senza distruggere i pori/incisioni; memoria e latenza misurate.

## 3. Adaptive Sculpt LOD

- **Non** confondere LOD runtime con semplici mesh decimate. Il dato di sculpt a qualità piena è preservato.
- Distinzione esplicita di: `display LOD` per disegno e `edit precision` per geometria realmente colpita dal brush.
- Selezione di LOD regionale con screen-space error, silhouette, dimensioni del pennello e isteresi.
- Promozione temporanea della zona attiva alla densità necessaria; seamless/crack-free tra griglie con livelli differenti.
- Aggiornamento della sola regione modificata; layer, maschere, pose symmetry e microdetail coerenti.
- **Criterio di successo:** nessuna perdita di dettaglio né cambiamento della scultura finale a parità di stroke, e benefici misurati anziché presunti.

## Surface Projection — componente esistente, non nuovo sviluppo prioritario

Secondo conferma del proprietario del progetto, **Surface Projection è già presente nella Build**. Non creare un duplicato. Prima dell'integrazione con i tre sistemi:
- verificare percorso, interfaccia, stato tecnico e test della specifica implementazione nel branch effettivamente usato;
- assicurarsi che pose, cambi di LOD e microdetail non provochino slittamento delle coordinate, artefatti alle cuciture o perdita di continuità;
- mantenere proiezione e advection indipendenti dalla risoluzione mostrata, dove possibile.

**Nota sullo stato:** questa annotazione registra una funzionalità dichiarata presente dall'utente; non costituisce verifica del codice relativo a Surface Projection.

## Vincoli e strategia di sviluppo

- Non modificare direttamente il checkpoint Windows 5.2.3 approvato il 2026-10-28.
- Sviluppo in branch sperimentali separati, attivazione opt-in e rollback disponibile.
- Test con mesh semplificate, poi con scene creature/character dense; misurare RAM/VRAM, time-to-first-stroke, latency e frame time.
- Verificare Windows e Linux individualmente; le release correnti non condividono necessariamente lo stesso source checkpoint.
- Non attribuire velocità o risparmi di memoria senza benchmark riproducibili.
- Mantenere l'approccio nativo in Blender; nessun passaggio obbligatorio tra Sculpt e Pose/Edit Mode per Quick Pose.

## Fonti di ispirazione / valutazione

- ZBrush: Poseable Symmetry, morph/pose sculpt workflow.
- SculptForge: microdetail e storage compatto, Multires e Selection Sets come riferimento progettuale. Le architetture sono diverse: non assumere compatibilità del codice.
- Architettura interna Blender: Multires CCG, PBVH e Draw Transport attuali; aggiornamenti incrementali.

Ultimo checkpoint verificato nella documentazione: [ottimizzazioni 2026-10-28](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/233b1a2da926d9200da5321674e54038875813b1/doc/sculpt_extended/CHECKPOINT_20261028.md).
