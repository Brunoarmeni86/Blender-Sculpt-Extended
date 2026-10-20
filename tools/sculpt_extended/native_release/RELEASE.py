"""Apply the reviewed native-startup integration, build/install Windows, test, package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import zipfile

KIT = Path(__file__).resolve().parent


def normalized(path):
    return path.read_text(encoding='utf-8-sig').replace('\r\n', '\n')


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(4*1024*1024), b''):
            value.update(chunk)
    return value.hexdigest()


def command(args, log, env=None, timeout=None):
    print('ESEGUO: '+subprocess.list2cmdline([str(x) for x in args]), flush=True)
    with log.open('w', encoding='utf-8') as output:
        try:
            proc = subprocess.Popen([str(x) for x in args], stdout=output,
                                    stderr=subprocess.STDOUT, env=env)
            start = time.monotonic()
            while proc.poll() is None:
                elapsed = int(time.monotonic()-start)
                print('  In corso: %ds | log: %s' % (elapsed, log), flush=True)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                if timeout and time.monotonic()-start > timeout and proc.poll() is None:
                    subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    raise RuntimeError('Timeout: '+str(log))
            code = proc.returncode
        except BaseException:
            if 'proc' in locals() and proc.poll() is None:
                subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            raise
    if code:
        print(log.read_text(encoding='utf-8', errors='replace')[-12000:], flush=True)
        raise RuntimeError('Comando fallito (%d): %s' % (code, log))


def apply_changes(source, build, run):
    changes = {}
    for fragment in json.loads((KIT/'FRAGMENTS.json').read_text()):
        name = fragment['path']
        if name not in changes:
            old = normalized(source/name)
            changes[name] = [old, old]
        text = changes[name][1]
        if text.count(fragment['after']) == fragment['count']:
            continue
        if text.count(fragment['before']) != fragment['count']:
            raise RuntimeError('Contesto sorgente differente: '+fragment['label'])
        changes[name][1] = text.replace(fragment['before'], fragment['after'])
    for row in json.loads((KIT/'NEW-FILES.json').read_text()):
        payload = KIT/'payload'/row['name']
        if digest(payload) != row['sha256']:
            raise RuntimeError('Payload danneggiato: '+row['name'])
        target = source/row['path']
        new = normalized(payload)
        old = normalized(target) if target.exists() else None
        if old is not None and old != new:
            raise RuntimeError('Nuovo file gia presente ma differente: '+str(target))
        changes[row['path']] = [old, new]
    # Complete every context/manifest check before writing any source.
    expected = json.loads((KIT/'BASELINE.json').read_text())
    for row in expected:
        path = source/row['path']
        if hashlib.sha256(normalized(path).encode()).hexdigest() != row['sha256']:
            # Accept only the exact native version installed by this kit.
            if row['path'] not in changes or normalized(path) != changes[row['path']][1]:
                raise RuntimeError('Baseline modificata: '+row['path'])
    backup = run/'backup'
    backup.mkdir()
    old_exe = build/'bin/Release/blender.exe'
    shutil.copy2(old_exe, backup/'blender.exe')
    if digest(old_exe) != digest(backup/'blender.exe'):
        raise RuntimeError('Backup eseguibile non valido')
    restore = []
    for name, (old, new) in changes.items():
        if old == new:
            continue
        destination = backup/name
        if old is not None:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source/name, destination)
        restore.append(dict(path=name, existed=old is not None,
                            installed_sha256=hashlib.sha256(new.encode()).hexdigest()))
    (backup/'RESTORE.json').write_text(json.dumps(dict(source=str(source), build=str(build),
         exe_sha256=digest(backup/'blender.exe'), files=restore), indent=2))
    for name, (old, new) in changes.items():
        if old != new:
            target = source/name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(new, encoding='utf-8', newline='\n')
    print('Backup: '+str(backup), flush=True)


def restore(backup):
    data = json.loads((backup/'RESTORE.json').read_text())
    source = Path(data['source'])
    for row in data['files']:
        if digest(source/row['path']) != row['installed_sha256']:
            raise RuntimeError('File cambiato dopo la patch: '+row['path'])
        if row['existed'] and not (backup/row['path']).is_file():
            raise RuntimeError('Backup incompleto')
    if digest(backup/'blender.exe') != data['exe_sha256']:
        raise RuntimeError('Hash backup exe non valido')
    for row in data['files']:
        target = source/row['path']
        if row['existed']:
            shutil.copy2(backup/row['path'], target)
        else:
            target.unlink()
    shutil.copy2(backup/'blender.exe', Path(data['build'])/'bin/Release/blender.exe')
    print('Sorgenti modificati ed eseguibile precedente ripristinati.', flush=True)


def main(args):
    if os.name != 'nt':
        raise RuntimeError('Eseguire questo strumento sul Windows della build.')
    # Avoid WMI/CIM: those queries stalled on the user's machine.
    processes = subprocess.check_output(['tasklist', '/FO', 'CSV', '/NH'], text=True, errors='replace').lower()
    for name in ['blender.exe', 'cmake.exe', 'ninja.exe', 'cl.exe', 'link.exe']:
        if '"'+name+'"' in processes:
            raise RuntimeError('Chiudi Blender o attendi la build: '+name)
    if args.restore:
        restore(args.restore)
        return
    source, build = args.source.resolve(), args.build.resolve()
    cache = {}
    for line in (build/'CMakeCache.txt').read_text(encoding='utf-8', errors='replace').splitlines():
        if '=' in line and not line.startswith(('#', '//')):
            key, value = line.split('=', 1)
            cache[key.split(':')[0]] = value
    if Path(cache['CMAKE_HOME_DIRECTORY']).resolve() != source:
        raise RuntimeError('Il cache CMake usa altri sorgenti.')
    cmake = Path(cache['CMAKE_COMMAND'])
    if not cmake.is_file():
        raise RuntimeError('CMake del cache non trovato.')
    run = KIT/('release-'+time.strftime('%Y%m%d-%H%M%S'))
    run.mkdir()
    print('[1/6] Verifica e backup dei sorgenti...', flush=True)
    apply_changes(source, build, run)
    print('[2/6] Compilazione Release con configurazione esistente...', flush=True)
    command([cmake, '--build', build, '--config', 'Release', '--parallel', str(args.parallel)], run/'build.log')
    print('[3/6] Installazione completa di eseguibile, librerie e risorse...', flush=True)
    portable = run/'Blender-Sculpt-Extended-Windows-x64'
    command([cmake, '--install', build, '--config', 'Release', '--prefix', portable], run/'install.log')
    exe = portable/'blender.exe'
    ui = list(portable.glob('*/scripts/startup/bl_ui/space_view3d_sculpt_extended.py'))
    python = list(portable.glob('*/python/bin/python.exe'))
    if not exe.is_file() or not ui or not python:
        raise RuntimeError('Installazione incompleta: exe, UI o Python mancante. Vedi install.log')
    if digest(ui[0]) != digest(KIT/'payload/space_view3d_sculpt_extended.py'):
        raise RuntimeError('La UI installata non corrisponde alla candidata.')
    (portable/'portable').mkdir(exist_ok=True)
    shutil.copy2(source/'COPYING', portable/'COPYING')
    print('[4/6] Test nativo senza launcher o flag di attivazione esterni...', flush=True)
    env = os.environ.copy()
    for key in list(json.loads((KIT/'PROFILE.json').read_text()))+['BLENDER_SCULPT_EXTENDED_NATIVE']:
        env.pop(key, None)
    env['BLENDER_SCULPT_POLYPAINT_L0_TRACE'] = '1'
    case_root = run/'native-test'
    (case_root/'logs').mkdir(parents=True)
    env['SUITE_CASE'] = 'l0_display'
    env['SUITE_CASE_ROOT'] = str(case_root)
    results = []
    for phase in ['save', 'reopen']:
        env['FACESETS_RC7_PHASE'] = env['FACESETS_RC8_PHASE'] = phase
        launch = [exe, '--factory-startup']
        if phase == 'reopen':
            launch.append(case_root/'logs/case.blend')
        launch.extend(['--python', KIT/'CASO.py'])
        command(launch, run/('native-'+phase+'.log'), env=env, timeout=600)
        report = json.loads((case_root/'logs'/('result-'+phase+'.json')).read_text())
        if report['status'] != 'PASS':
            raise RuntimeError('Test nativo fallito: '+phase)
        results.append(report)
    text = (run/'native-save.log').read_text(encoding='utf-8', errors='replace')
    if '[POLYPAINT_L0_WORKBENCH_V04E] sculpt_pbvh=1' not in text or '[NATIVE_RELEASE] PASS' not in text:
        raise RuntimeError('Mancano le tracce del percorso nativo verificato.')
    print('[5/6] Manifest e preparazione pacchetto...', flush=True)
    commit = (KIT/'GITHUB-COMMIT.txt').read_text().strip() if (KIT/'GITHUB-COMMIT.txt').exists() else 'pending'
    final = dict(status='PASS_NATIVE_SHAREABLE', exe_sha256=digest(exe), github_commit=commit,
                 results=results, gpu_config={key:value for key,value in cache.items() if key.startswith('WITH_CYCLES_DEVICE_')},
                 scope='Native defaults/UI startup; L0 viewport, read-only invariants, Paint residual, Undo/Redo, save/reopen. Existing GPU settings retained, not GPU-runtime tested.')
    (run/'RISULTATO-RELEASE.json').write_text(json.dumps(final, indent=2))
    (portable/'SCULPT-EXTENDED-RELEASE.json').write_text(json.dumps(final, indent=2))
    print('[6/6] Compressione shareable completa...', flush=True)
    archive = run/'Blender-Sculpt-Extended-Windows-x64.zip'
    files = [path for path in portable.rglob('*') if path.is_file()]
    total, done = sum(path.stat().st_size for path in files), 0
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=1) as stored:
        for index, path in enumerate(files, 1):
            stored.write(path, str(Path(portable.name)/path.relative_to(portable)))
            done += path.stat().st_size
            if index % 100 == 0 or index == len(files):
                print('  ZIP %d%% | file %d/%d' % (100*done/max(total, 1), index, len(files)), flush=True)
    with zipfile.ZipFile(archive) as stored:
        if stored.testzip():
            raise RuntimeError('Verifica ZIP fallita')
    (run/'SHA256.txt').write_text(digest(archive)+'  '+archive.name+'\n'+digest(exe)+'  blender.exe\n')
    print('COMPLETATO: '+str(archive), flush=True)
    print('Avvio diretto: '+str(exe), flush=True)
    print('Invia RISULTATO-RELEASE.json e SHA256.txt da '+str(run), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(r'C:\BlenderDev\blender'))
    parser.add_argument('--build', type=Path, default=Path(r'C:\BlenderDev\build_windows_x64_shareable'))
    parser.add_argument('--parallel', type=int, choices=range(1, 65), default=4)
    parser.add_argument('--restore', type=Path)
    args = parser.parse_args()
    try:
        main(args)
    except Exception as error:
        print('ERRORE: '+str(error), file=sys.stderr, flush=True)
        sys.exit(1)
