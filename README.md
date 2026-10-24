# Blender Sculpt Extended

Custom build di Blender per lo sculpt su mesh Multires dense, con Sculpt Layers, Preserve Other Layers e Polypaint.

## Release corrente: Blender Sculpt Extended 5.2.3

| Sistema | Download | Revisione e verifiche |
|---|---|---|
| Windows x64 | [Scarica ZIP](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview/Blender-Sculpt-Extended-5.2.3-Windows-x64.zip) | `7eb49ddcb0d`; compilazione, avvio e prove locali |
| Linux x64 general purpose | [Scarica TAR.XZ](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview/Blender-Sculpt-Extended-5.2.3-Linux-x64.tar.xz) | `7eb49ddcb0d`; Rocky Linux 8 / glibc 2.28, verificata anche su Ubuntu 24.04 |
| Ubuntu 24.04 x64, alternativa | [Scarica TAR.XZ Ubuntu24](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview/Blender-Sculpt-Extended-5.2.3-Linux-x64-Ubuntu24.tar.xz) | Precedente revisione `d62af38`, conservata come alternativa |

Windows e Linux general purpose includono la stessa interfaccia Multires compatta e i fix approvati di maschere e Face Sets. Consulta le [note della release](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview) per i dettagli e lo stato dei problemi ancora in indagine. Checksum SHA-256 e manifest sono allegati alla release.

Linux general purpose include i backend e kernel Cycles CUDA/OptiX, HIP e oneAPI. Il pacchetto è stato verificato su Rocky Linux 8 e Ubuntu 24.04; l'esecuzione su GPU fisiche non è stata provata dal runner CI. La compatibilità richiede anche CPU, sistema grafico e driver supportati; glibc 2.28 è la baseline ABI.

## Funzioni principali

- Sculpt Layers per Multires con gruppi e interfaccia compatta.
- Preserve Other Layers sui pennelli supportati, compresi Space e Anchored.
- Polypaint e bake UV integrato nei pannelli Multires.
- Base Smooth, visualizzazione Multires e ottimizzazioni per mesh dense.
- Face Sets e propagazione delle maschere tra base e livelli Multires.

Le release indicate come **Superseded** sono archiviate. Questi pacchetti restano pre-release personalizzate.

## Sorgenti e documentazione

- [Sorgente esatto dei pacchetti principali](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/7eb49ddcb0df065d12129df06134437d795b5c70)
- [Release nativa e verifiche](doc/sculpt_extended/NATIVE_RELEASE_20261020.md)
- [Audit del codice](doc/sculpt_extended/CODE_REVIEW.md)

## Blender e licenza

Blender è una suite 3D libera e open source: [sito ufficiale](https://www.blender.org), [manuale](https://docs.blender.org/manual/en/latest/index.html) e [licenza](https://www.blender.org/about/license/). Blender è distribuito principalmente secondo GNU GPL versione 3; singoli file possono usare licenze compatibili differenti.
