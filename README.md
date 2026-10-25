# Blender Sculpt Extended

Build personalizzata di Blender per sculpt Multires su mesh dense, con Sculpt Layers, Preserve Other Layers e Polypaint.

## Versione principale: Blender Sculpt Extended 5.2.3 — Ultime correzioni

[**Apri la release consigliata**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-smooth-l0-20261025-preview) · [Tutte le versioni](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases)

| Sistema | Download | Revisione e stato |
|---|---|---|
| **Windows x64** | [**Scarica ZIP portable**](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-smooth-l0-20261025-preview/Blender-Sculpt-Extended-5.2.3-Windows-x64.zip) | `08db801d6a5`; include Smooth L0, OpenSubdiv e RNA |
| **Linux x64 general purpose** | [Compilazione sospesa](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/actions/runs/40201092229) | Stesso sorgente `08db801d6a5`; pubblicata dopo verifica |
| Linux general purpose precedente | [Scarica Clear Mask](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-clear-mask-20261024-preview/Blender-Sculpt-Extended-5.2.3-Linux-x64.tar.xz) | `ab6dd77b95c`; non include le ultime correzioni Smooth L0/RNA |
| Ubuntu 24.04, alternativa storica | [Scarica Ubuntu24](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview/Blender-Sculpt-Extended-5.2.3-Linux-x64-Ubuntu24.tar.xz) | `d62af38`; precedente anche al fix Clear Mask |

La Windows attuale comprende tutte le correzioni precedenti e le nuove modifiche a Smooth/Shift con layer L0, riuso dei riferimenti Multires e correzioni OpenSubdiv/RNA. Checksum e manifest sono allegati alla release. Le build storiche restano disponibili e sono contrassegnate come tali; le revisioni diverse non vanno considerate equivalenti.

## Funzioni principali

- Sculpt Layers per Multires con gruppi e interfaccia compatta in stile Shape Keys.
- Preserve Other Layers sui pennelli supportati, Space e Anchored; Smooth L0 richiede Multires primo nello stack.
- Polypaint e bake UV integrati nei pannelli Multires.
- Face Sets, propagazione maschere e fix Clear Mask/Subdivide.
- Preparazione dei riferimenti e buffer ottimizzati per mesh dense. Pose e Grab restano standard.

Linux general purpose usa Rocky Linux 8/glibc 2.28 e funziona anche su Ubuntu compatibile. Sono inclusi CUDA/OptiX, HIP e oneAPI nelle build verificate; il runner non esegue prove su GPU fisiche. Servono CPU, sistema grafico e driver supportati. L'alternativa Ubuntu24 conserva una revisione precedente.

## Sorgenti e verifiche

- [Sorgente esatto della nuova versione](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/08db801d6a539d0737aa29f6811644d2bb06a6d9)
- [Correzioni, verifiche e limiti Smooth L0/RNA](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/blob/08db801d6a539d0737aa29f6811644d2bb06a6d9/doc/sculpt_extended/SMOOTH_L0_20261025.md)
- [Audit del codice](doc/sculpt_extended/CODE_REVIEW.md)

Build personalizzata del progetto, non distribuzione ufficiale Blender. [Blender](https://www.blender.org) è software libero, principalmente sotto GNU GPL v3; i singoli file possono avere licenze compatibili differenti.
