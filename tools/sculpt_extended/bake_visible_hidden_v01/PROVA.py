"""Bake Visible hidden-layer fix and native Multires sidebar integration."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import zipfile
import BUILD_HELPER as helper

KIT = Path(__file__).resolve().parent
helper.KIT = KIT
CURRENT = 'a00113514db414c317fa6a535e45c8bd08931ade8f9bcf4066a883d880a5ccdd'
STAGE = 'Blender-Sculpt-Bake-Visible-Hidden-V01'


def integrity():
    data = json.loads((KIT / 'INTEGRITA.json').read_text())
    for name, wanted in data['files'].items():
        path = KIT / name
        if not path.is_file() or helper.sha(path) != wanted:
            raise RuntimeError('File del kit assente o modificato: ' + name)


def cache(path):
    data = {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if '=' in line and not line.startswith(('#', '//')):
            key, value = line.split('=', 1)
            data[key.split(':')[0]] = value
    return data


def env(regional=False, stable=False):
    result = helper.environment('unchanged_candidate')
    result['BLENDER_SCULPT_SUBDIVIDE_TOPOLOGY_V03'] = '0'
    result['BLENDER_SCULPT_SUBDIVIDE_BASE_PHASE_V03'] = '0'
    result['BLENDER_SCULPT_SUBDIVIDE_REGIONAL_V03'] = '1' if regional else '0'
    result['BLENDER_MULTIRES_DETAIL_TRANSPORT'] = 'rotation' if stable else 'affine'
    result.pop('BLENDER_SCULPT_REGION_CAPTURE_V02', None)
    return result


def restore_original(run):
    backup = run / 'backup'
    if not (backup / 'RESTORE.json').exists():
        return
    data = json.loads((backup / 'RESTORE.json').read_text())
    source = Path(data['source'])
    # Check every original and destination before changing anything.
    for row in data['files']:
        target, saved = source / row['path'], backup / row['path']
        if row['existed'] and helper.sha(saved) != row['before_bytes_sha256']:
            raise RuntimeError('Backup da verificare: ' + str(saved))
        if not target.exists() and not row['existed']:
            continue
        if (not row['existed'] or helper.sha(target) != row['before_bytes_sha256']) and helper.text_sha(target) != row['after_text_sha256']:
            raise RuntimeError('Sorgente modificato da altri durante la prova: ' + str(target))
    if helper.sha(backup / 'blender.exe') != data['exe_sha256']:
        raise RuntimeError('Backup eseguibile da verificare')
    for row in data['files']:
        target = source / row['path']
        if row['existed']:
            shutil.copy2(backup / row['path'], target)
            target.touch()  # Invalidate objects compiled against the expanded context.
        else:
            target.unlink(missing_ok=True)
    destination = Path(data['build']) / 'bin/Release/blender.exe'
    shutil.copy2(backup / 'blender.exe', destination)
    if helper.sha(destination) != data['exe_sha256']:
        raise RuntimeError('Ripristino eseguibile non verificato')
    for row in data['files']:
        if (row['existed'] and helper.sha(source / row['path']) != row['before_bytes_sha256']) or (not row['existed'] and (source / row['path']).exists()):
            raise RuntimeError('Ripristino sorgente non verificato')
    (run / 'RIPRISTINO.json').write_text(json.dumps(dict(status='RESTORED', exe_sha256=data['exe_sha256'], files=len(data['files'])), indent=2))
    print('Ripristinati e verificati sorgente modificato ed exe precedente.', flush=True)


def test(run, candidate):
    root = run / 'test'
    root.mkdir()
    results = {}
    for case in ['HR', 'L0']:
        folder = root / case
        folder.mkdir()
        results[case] = {}
        for action in ['seed', 'exercise', 'reopen']:
            environment = env(regional=True)
            environment.update(SUBDIV_TEST_ROOT=str(folder), SUBDIV_TEST_PHASE=action,
                               SUBDIV_SEED=str(folder / 'seed.blend'), SUBDIV_REFERENCE='',
                               BAKE_L0='1' if case == 'L0' else '0')
            helper.command([candidate, '--factory-startup', '--no-window-focus', '--python', KIT / 'WORKER.py'],
                           folder / (action + '.log'), env=environment, timeout=900)
            data = json.loads((folder / (action + '.json')).read_text())
            if not data.get('status', '').startswith('PASS'):
                raise RuntimeError(data.get('error', case + ' ' + action + ' incompleto'))
            results[case][action] = data
    return dict(status='PASS_BAKE_VISIBLE_HIDDEN', tests=results)


def collect(run):
    with zipfile.ZipFile(run / 'LOG-DA-INVIARE.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        if (run / 'backup/RESTORE.json').is_file():
            archive.write(run / 'backup/RESTORE.json', 'backup/RESTORE.json')
        archive.write(KIT / 'PATCH.json', 'PATCH-APPLICATA.json')
        # Never walk the installed portable/Python or compiler output tree.
        paths = list(run.glob('*.json')) + list(run.glob('*.log'))
        if (run / 'test').is_dir():
            paths += list((run / 'test').rglob('*'))
        for path in sorted(set(paths)):
            rel = path.relative_to(run)
            if path.is_file() and rel.parts[0] not in ('backup', 'build-replay', STAGE) and path.suffix in ('.json', '.log', '.txt'):
                archive.write(path, rel)
    print('LOG DA INVIARE: ' + str(run / 'LOG-DA-INVIARE.zip'), flush=True)


def main(args):
    if os.name != 'nt':
        raise RuntimeError('Eseguire sul Windows della build.')
    helper.idle()
    if args.restore:
        restore_original(args.restore.resolve())
        return
    integrity()
    if args.launch:
        run = Path((KIT / 'ULTIMA.txt').read_text().strip())
        report = json.loads((run / 'RISULTATO.json').read_text())
        exe = run / STAGE / 'blender.exe'
        if report.get('status') != 'PASS_BAKE_VISIBLE_HIDDEN' or helper.sha(exe) != report.get('candidate_sha256'):
            raise RuntimeError('La prova Bake Visible completa non passa o la portable e cambiata.')
        subprocess.Popen([str(exe)], env=env(regional=True, stable=args.rotation))
        return
    if not 1 <= args.parallel <= 32:
        raise RuntimeError('--parallel deve essere fra 1 e 32')
    source, build = args.source.resolve(), args.build.resolve()
    if helper.sha(build / 'bin/Release/blender.exe') != CURRENT:
        raise RuntimeError('Build diversa dall’ultima V03 ricevuta: nessuna modifica applicata.')
    values = cache(build / 'CMakeCache.txt')
    if Path(values['CMAKE_HOME_DIRECTORY']).resolve() != source:
        raise RuntimeError('CMake punta a sorgenti diversi')
    cmake = Path(values['CMAKE_COMMAND'])
    generator = values.get('CMAKE_GENERATOR', '')
    if not cmake.is_file() or not generator.startswith('Visual Studio '):
        raise RuntimeError('CMake/Visual Studio non corrispondono alla build attesa')
    run = KIT / ('prova-' + time.strftime('%Y%m%d-%H%M%S'))
    run.mkdir()
    (KIT / 'ULTIMA.txt').write_text(str(run))
    state = dict(status='RUNNING', regional_execution_opt_in=True,
                 baseline_sha256=CURRENT)
    try:
        print('[1/4] Controllo sorgenti e backup verificato...', flush=True)
        print('Preparazione fix Bake Visible e conservazione regionale V03...', flush=True)
        try:
            helper.apply(source, build, run)
            print('[2/4] Compilazione Blender con Bake Visible corretto...', flush=True)
            helper.command([cmake, '--build', build, '--config', 'Release', '--parallel', str(args.parallel)], run / 'build.log')
            print('[3/4] Portable separata e ripristino build precedente...', flush=True)
            stage = run / STAGE
            helper.command([cmake, '--install', build, '--config', 'Release', '--prefix', stage], run / 'install.log')
            candidate = stage / 'blender.exe'
            if not candidate.is_file() or not list(stage.glob('*/python/bin/python.exe')):
                raise RuntimeError('Portable incompleta')
            (stage / 'portable').mkdir(exist_ok=True)
            state['candidate_sha256'] = helper.sha(candidate)
        finally:
            restore_original(run)
        print('[4/4] Test Bake Visible con layer nascosti, Undo/Redo e riapertura...', flush=True)
        state.update(test(run, candidate))
        state['status'] = 'PASS_BAKE_VISIBLE_HIDDEN'
        print('PASS_BAKE_VISIBLE_HIDDEN: invia il log prima di usare la portable su progetti reali.', flush=True)
    except BaseException as exc:
        state.update(status='INTERRUPTED' if isinstance(exc, KeyboardInterrupt) else 'REVIEW_REQUIRED', error=str(exc))
        raise
    finally:
        state['original_restored'] = (run / 'RIPRISTINO.json').exists()
        (run / 'RISULTATO.json').write_text(json.dumps(state, indent=2))
        collect(run)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=Path(r'C:\BlenderDev\blender'))
    parser.add_argument('--build', type=Path, default=Path(r'C:\BlenderDev\build_windows_x64_shareable'))
    parser.add_argument('--parallel', type=int, default=4)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--restore', type=Path)
    modes.add_argument('--launch', action='store_true')
    parser.add_argument('--rotation', action='store_true')
    try:
        main(parser.parse_args())
    except KeyboardInterrupt:
        print('Interrotto: controllare RISULTATO.json e RIPRISTINO.json nella cartella prova.', flush=True)
        raise SystemExit(130)
    except Exception as exc:
        print('ERRORE: ' + str(exc), flush=True)
        raise SystemExit(1)
