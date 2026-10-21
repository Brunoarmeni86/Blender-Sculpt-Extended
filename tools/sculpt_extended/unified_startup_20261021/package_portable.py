"""Prepare a runtime-only portable from an installed Blender build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--installed', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--archive', type=Path, required=True)
parser.add_argument('--source-commit', required=True)
parser.add_argument('--manifest', type=Path)
args = parser.parse_args()
source = args.installed.resolve(strict=True)
target = args.output.resolve()
assert not target.exists(), f'Output already exists: {target}'
assert source != target and not target.is_relative_to(source)
assert (source / 'blender.exe').is_file()
target.mkdir(parents=True)
excluded_root_dirs = {'SculptExtended'}
excluded_tools = {'datatoc.exe', 'makesdna.exe', 'makesrna.exe', 'msgfmt.exe', 'shader_tool.exe', 'zstd_compress.exe'}
excluded_suffixes = {'.pdb', '.lib', '.exp', '.ilk', '.obj', '.pyc'}
for base, dirs, files in os.walk(source, followlinks=False):
    rel = Path(base).relative_to(source)
    dirs[:] = [name for name in dirs if name != '__pycache__' and '.bak' not in name.lower()
               and not (rel == Path('.') and name in excluded_root_dirs)]
    for name in dirs:
        path = Path(base) / name
        assert not path.is_symlink() and not os.path.isjunction(path), path
    for name in files:
        path = Path(base) / name
        if path.suffix.lower() in excluded_suffixes or '.bak' in name.lower():
            continue
        if rel == Path('.') and name in excluded_tools:
            continue
        assert not path.is_symlink(), path
        destination = target / rel / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
manifest = json.loads((args.manifest or target / 'BUILD-MANIFEST.json').read_text())
manifest['source_commit'] = args.source_commit
manifest['source_state'] = 'committed native-startup fix; installed build checked against source hashes'
manifest['distribution'] = 'runtime-only portable; no SculptExtended launcher scripts or development symbols'
(target / 'BUILD-MANIFEST.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
(target / 'LEGGIMI.txt').write_text(
    'Blender Sculpt Extended\n\nAvvia blender.exe: tutte le funzioni Sculpt Extended sono native.\n'
    'AVVIA-SCULPT-EXTENDED.cmd avvia lo stesso eseguibile, senza profili aggiuntivi.\n'
    'Codice sorgente e licenza: https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/'
    + args.source_commit + '\n\nVedi BUILD-MANIFEST.json per hash e verifiche.\n', encoding='utf-8')
temporary = args.archive.with_suffix('.zip.partial')
assert not temporary.exists()
with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    for path in sorted(target.rglob('*')):
        if path.is_file():
            archive.write(path, (Path(args.archive.stem) / path.relative_to(target)).as_posix())
with zipfile.ZipFile(temporary) as archive:
    assert archive.testzip() is None
    for item in archive.infolist():
        if item.is_dir():
            continue
        path = target / Path(*Path(item.filename).parts[1:])
        with archive.open(item) as packed, path.open('rb') as original:
            assert hashlib.file_digest(packed, 'sha256').digest() == hashlib.file_digest(original, 'sha256').digest(), path
temporary.replace(args.archive)
print(f'Verified portable: {target}\nZIP: {args.archive}', flush=True)
