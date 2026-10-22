# Distinzione delle ottimizzazioni — 21 ottobre 2026

## Profilo approvato

La baseline è il profilo V05 congelato: 62 impostazioni con valori esatti, non 62 funzioni tutte attive. Include Subdivide regionale/Combined V05, preparazione parallela, scratch, riuso coarse, cache/supporto layer, delta incrementali, Polypaint, Draw Transport e Face Sets approvati. Fonte: `Sculpt-Extended-Complete-V05/NATIVE-PROFILE.hh`, identica alla copia della build completa SE5. SHA256 ac30993e98b4420afd5edb60aa2202b18b76883ff8255aa7f9c30a78969684b7.

## Esclusioni documentate

| Opzione | Decisione | Motivo / prova |
|---|---|---|
| COMMIT_KEEP_CCG | 0 | Problemi geometrici; vincolo mantenuto nella V05 congelata |
| Shared-Work: BASE_PHASE_V03, BASE_CACHE_MB_V03, TOPOLOGY_V03 | 0 | Vecchia cache globale più lenta; distinto dal riuso topologico locale Combined V05 che resta attivo |
| Face Sets DENSE_V06D, MESH_TRIAL_V06E e tre trial MULTIRES_FACESETS | 0 | Esclusi dal profilo V05; non dedurre che ogni trial sia stato bocciato per lo stesso motivo |
| Architettura sparse blocks / V06 | Separata | Progetto sperimentale successivo, non parte della baseline accettata |
| REC | Rimosso dalla UI | Registrazione automatica; campo dati conservato per compatibilità dei file |
| Tracing e profiling | Non forzati | Diagnostica, non ottimizzazioni da accendere nella distribuzione |

Altri valori da conservare esattamente: FRAME_CACHE=1, EMPTY_LAYER_SUBDIVIDE=0, LAYER_CACHE_MB=256, PRESERVE_RETAINED=0, WEIGHT_MULTI_CACHE=0. Questi prevalgono sui valori differenti del vecchio launcher. Tutte le impostazioni a zero sono elencate nel manifest PROFILE-MATRIX.json; non attribuiamo una motivazione non documentata a ciascuna.

## Vecchio launcher: da verificare, non automaticamente approvato

28 impostazioni non presenti nel profilo congelato sono elencate in `unapproved_launcher_settings` del manifest. Comprendono geometry buffer, startup reuse, CCG full batch, compact commit e diversi controlli Undo/anchor. Non risultano tutte esplicitamente rifiutate, ma la loro presenza nel launcher non ne prova l'accettazione. Non sono aggiunte come default nativi. Alcuni percorsi possono essere già attivi come default nel codice: assenza della variabile non significa funzione rimossa.

Il candidato con 90 impostazioni ha passato 26 controlli di compatibilità; ciò non certifica un guadagno prestazionale né autorizza il recupero delle opzioni storiche. Il profilo esatto V05 ha passato separatamente gli stessi 26 controlli, con esecuzione effettiva dei percorsi regionali/Combined e cache globale spenta. Risultati conservati separatamente.

## Tracciabilità delle decisioni

`doc/sculpt_extended/FROZEN_V05_20261021.md` documenta baseline, KEEP_CCG e cache globale esclusa. La chat backend `01a14da3-0f1d-7140-9151-6e616f97a06b` conferma cache più lenta, approvazione V05 e progetto sparse separato. La chat `01a1179b-4f1e-7244-86b1-d6451970805b` richiede di preservare Combined/regional, parallel preparation/mask scratch e Draw Transport nel checkpoint UI.

## Correzioni concordate e verifiche finali

Il profilo nativo contiene esattamente i 62 valori congelati. La UI usa la sola tab Multires: Multiresolution e Sculpt Layers non sono duplicati in un secondo pannello Sculpt Extended. Draw Transport è un pannello figlio; Bake Visible mantiene il proprio operatore, distinto da Merge Visible. Face Sets HR e Polypaint Blur sono registrati all'avvio. REC è assente dalla UI e lasciato falso dalle fixture durante le pennellate reali. Nessuna modifica ai kernel geometrici o alle opzioni storiche escluse.

La regressione finale della UI consolidata passa Bake HR e L0, Subdivide affine e rotation, con seed, exercise e reopen. Comprende Undo/Redo, ulteriori pennellate, layer nascosti e verifiche di stato; i log confermano l'esecuzione regionale/Combined con base_cache=0. Passano inoltre avvio normale, override e opt-out. La precedente verifica del profilo esatto contro l'eseguibile V05 completo è conservata separatamente.

I confronti preliminari delle pennellate non dimostrano un rallentamento ripetibile imputabile alle 28 impostazioni storiche. Nessuna modifica prestazionale viene introdotta sulla base di quei tempi. La compilazione resta Release /O2 con TBB. Non viene promessa equivalenza prestazionale su scene, pennelli o hardware non misurati.

La portable pubblicata fa5 resta una prerelease con difetti noti fino alla sostituzione esplicita dei suoi allegati. Il nuovo manifest della distribuzione corretta deve identificare il commit e l'hash dell'eseguibile.

## Delta Smooth su Base Mesh — 22 ottobre 2026

Il percorso Delta Smooth dei layer ora viene ignorato quando `Sculpt Base Mesh` è il target. In questo caso Blender usa lo Smooth standard anche se tutti gli Sculpt Layers sono nascosti o hanno strength zero. Il controllo di layer visibile, attivo e con strength non zero resta invariato quando si scolpisce su un layer.

Verifica mirata: vera pennellata Smooth su Base Mesh con un layer nascosto a strength zero, operatore completato, nessun errore Delta Smooth e variazione geometrica della base confermata. Sono passati anche avvio, Bake HR/L0 e il confronto Subdivide affine; la suite più estesa è stata interrotta perché non necessaria per questa modifica circoscritta.
