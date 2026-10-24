# Blender Sculpt Extended

Custom build di Blender dedicata allo sculpt su mesh Multires dense, con Sculpt Layers, Preserve Other Layers, Polypaint e ottimizzazioni mirate.

## Release corrente

**[Blender Sculpt Extended 5.2.3 - download e note](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/tag/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview)**

| Sistema | Download | Indicazione |
|---|---|---|
| Windows x64 | [Scarica ZIP](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview/Blender-Sculpt-Extended-5.2.3-Windows-x64.zip) | Build completa e verificata |
| Linux x64 general purpose | [Scarica TAR.XZ](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview/Blender-Sculpt-Extended-5.2.3-Linux-x64.tar.xz) | **Consigliata**; Rocky Linux 8 / glibc 2.28, verificata anche su Ubuntu 24.04 |
| Linux x64 Ubuntu 24.04 | [Scarica TAR.XZ alternativo](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases/download/sculpt-extended-5.2.3-preserve-layers-fix-20261023-preview/Blender-Sculpt-Extended-5.2.3-Linux-x64-Ubuntu24.tar.xz) | Compilazione precedente specifica Ubuntu |

La build Linux general purpose include CUDA/OptiX per NVIDIA, HIP per AMD e oneAPI per Intel. I kernel sono presenti e l'avvio è verificato; il runner non disponeva di GPU fisiche per misurare rendering e prestazioni reali.

Le release contrassegnate **Superseded** sono conservate come archivio e non sono consigliate. Consulta la [pagina Releases](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/releases) per lo stato aggiornato.

## Funzioni principali

- Sculpt Layers per Multires.
- Preserve Other Layers sui pennelli supportati.
- Polypaint e strumenti di trasferimento dei dettagli.
- Base Smooth e visualizzazione Multires coerente tra Sculpt Mode e Object Mode.
- Miglioramenti prestazionali per lo sculpt su mesh dense.

## Documentazione tecnica

- [Release nativa e verifiche](doc/sculpt_extended/NATIVE_RELEASE_20261020.md)
- [Audit del codice, difetti e ottimizzazioni](doc/sculpt_extended/CODE_REVIEW.md)
- [Sorgente esatto della release 5.2.3](https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/d62af38df29b687ecf6a10985b032b6694a062c5)

## Blender e licenza

Blender è una suite 3D libera e open source. Consulta il [sito ufficiale](https://www.blender.org), il [manuale](https://docs.blender.org/manual/en/latest/index.html) e la [licenza](https://www.blender.org/about/license/).
