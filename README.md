# Blender Sculpt Extended

Custom build di Blender per lo sculpt su mesh Multires dense, con Sculpt Layers, Preserve Other Layers e Polypaint.

## Release corrente: Blender Sculpt Extended 5.2.3 — Clear Mask fix

| Sistema | Download | Revisione e verifiche |
|---|---|---|
| Windows x64 | [Scarica ZIP](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-clear-mask-20261024-preview/Blender-Sculpt-Extended-5.2.3-Windows-x64.zip) | `ab6dd77b95c`; include il fix Clear Mask / Subdivide |
| Linux x64 general purpose | [Scarica TAR.XZ](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-clear-mask-20261024-preview/Blender-Sculpt-Extended-5.2.3-Linux-x64.tar.xz) | `ab6dd77b95c`; include il fix Clear Mask / Subdivide; verificata su Rocky 8 e Ubuntu 24.04 |
| Ubuntu 24.04 x64, alternativa | [Scarica TAR.XZ Ubuntu24](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview/Blender-Sculpt-Extended-5.2.3-Linux-x64-Ubuntu24.tar.xz) | Precedente revisione `d62af38`, conservata come alternativa |

Windows e Linux general purpose includono il fix Clear Mask / Subdivide e tutte le funzioni precedentemente approvate, sullo stesso sorgente `ab6dd77b95c`. La [verifica e pubblicazione Linux](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/actions/runs/40167961272) è completata. L'alternativa Ubuntu24 conserva la revisione precedente e non include l'ultimo fix Clear Mask. Consulta le [note della release](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-clear-mask-20261024-preview) per i dettagli e lo stato dei problemi ancora in indagine. Checksum SHA-256 e manifest sono allegati alla release.

Linux general purpose include i backend e kernel Cycles CUDA/OptiX, HIP e oneAPI. Il pacchetto è stato verificato su Rocky Linux 8 e Ubuntu 24.04; l'esecuzione su GPU fisiche non è stata provata dal runner CI. La compatibilità richiede anche CPU, sistema grafico e driver supportati; glibc 2.28 è la baseline ABI.

## Funzioni principali

- Sculpt Layers per Multires con gruppi e interfaccia compatta.
- Preserve Other Layers sui pennelli supportati, compresi Space e Anchored.
- Polypaint e bake UV integrato nei pannelli Multires.
- Base Smooth, visualizzazione Multires e ottimizzazioni per mesh dense.
- Face Sets e propagazione delle maschere tra base e livelli Multires.

Le release indicate come **Superseded** sono archiviate. Questi pacchetti restano pre-release personalizzate.

## Sorgenti e documentazione

- [Sorgente comune delle build Windows e Linux correnti](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/ab6dd77b95c47672a27b1be92c766c776559a435)
- [Release nativa e verifiche](doc/sculpt_extended/NATIVE_RELEASE_20261020.md)
- [Audit del codice](doc/sculpt_extended/CODE_REVIEW.md)

## Blender e licenza

Blender è una suite 3D libera e open source: [sito ufficiale](https://www.blender.org), [manuale](https://docs.blender.org/manual/en/latest/index.html) e [licenza](https://www.blender.org/about/license/). Blender è distribuito principalmente secondo GNU GPL versione 3; singoli file possono usare licenze compatibili differenti.
