#!/usr/bin/env python3
from pathlib import Path
import argparse
import hashlib
import json

parser = argparse.ArgumentParser()
parser.add_argument('backup', type=Path)
args = parser.parse_args()
manifest = json.loads((args.backup / 'manifest.json').read_text(encoding='utf-8'))
target = Path(manifest['target'])
header = Path(manifest['header'])
original = (args.backup / target.name).read_bytes()
sha = lambda payload: hashlib.sha256(payload).hexdigest()
if sha(original) != manifest['sha256']:
    raise SystemExit('Backup non integro. Nessuna modifica.')
if sha(target.read_bytes()) != manifest['patched_sha256']:
    raise SystemExit('Sorgente modificato dopo la patch: ripristino automatico rifiutato.')
if not header.exists() or sha(header.read_bytes()) != manifest['header_sha256']:
    raise SystemExit('Header modificato o mancante: ripristino automatico rifiutato.')
target.write_bytes(original)
header.unlink()
print('Sorgente originale ripristinato; ricompilare Blender.')
