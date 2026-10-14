"""Incremental Windows build + separate experimental portable; stdlib only."""
import argparse
from collections import deque
import datetime
import hashlib
import json
import ntpath
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time
import traceback
import uuid
import zipfile


def say(message):
    print(message, flush=True)


def windows_extended_path(value):
    """Use Win32 extended file paths, without changing global registry policy."""
    value = ntpath.normpath(os.fspath(value))
    if value.startswith('\\\\?\\'):
        return value
    if value.startswith('\\\\'):
        return '\\\\?\\UNC\\'+value[2:]
    if not ntpath.isabs(value):
        raise ValueError('Extended Windows paths require an absolute path')
    return '\\\\?\\'+value


def filesystem_path(path):
    if os.name == 'nt':
        return Path(windows_extended_path(os.path.abspath(os.fspath(path))))
    return Path(path)


def sha256(path, label=None):
    digest = hashlib.sha256()
    done = 0
    last = time.monotonic()
    with filesystem_path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
            done += len(block)
            if label and time.monotonic() - last >= 5:
                say(f'[{label}] letti {done/1024**3:.2f} GiB')
                last = time.monotonic()
    return digest.hexdigest()


def run_native(command, log, label, timeout=None, env=None, accepted=(0,)):
    """Drain native output without waiting forever for inherited pipe handles."""
    say(f'[{label}] avvio; log: {log}')
    messages = queue.Queue()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, text=True, errors='replace', env=env)

    def reader():
        try:
            for line in process.stdout:
                messages.put(line)
        finally:
            messages.put(None)

    threading.Thread(target=reader, daemon=True).start()
    start = heartbeat = time.monotonic()
    closed = False
    exited_at = None
    collected = deque(maxlen=1000)
    with filesystem_path(log).open('w', encoding='utf-8') as output:
        try:
            while True:
                try:
                    line = messages.get(timeout=0.2)
                    if line is None:
                        closed = True
                    else:
                        output.write(line)
                        output.flush()
                        say(line.rstrip())
                        collected.append(line)
                except queue.Empty:
                    pass
                now = time.monotonic()
                code = process.poll()
                if code is not None:
                    exited_at = now if exited_at is None else exited_at
                    if messages.empty() and (closed or now - exited_at > 2):
                        break
                if timeout and code is None and now - start > timeout:
                    process.kill()
                    process.wait(timeout=10)
                    raise TimeoutError(f'{label}: processo PID {process.pid} oltre {timeout}s. Nessuno ZIP finale; controllare {log}')
                if now - heartbeat >= 10:
                    say(f'[{label}] ancora in esecuzione, {int(now-start)}s, PID={process.pid}')
                    heartbeat = now
        except BaseException:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            raise
    if code not in accepted:
        raise RuntimeError(f'{label}: codice {code}. Log: {log}')
    say(f'[{label}] completato in {time.monotonic()-start:.1f}s; codice={code}')
    return code, ''.join(collected)


def copy_tree(source, destination, label, portable=False):
    source, destination = filesystem_path(source), filesystem_path(destination)
    files = []
    for item in source.rglob('*'):
        if item.is_symlink():
            raise RuntimeError(f'Collegamento da verificare prima di esportare: {item}')
        if item.is_file():
            relative = item.relative_to(source)
            # Only omit the user's per-version portable preferences in the export.
            if portable and len(relative.parts) >= 2 and relative.parts[0][:1].isdigit() and relative.parts[1].lower() == 'config':
                continue
            files.append((item, relative))
    total = sum(item.stat().st_size for item, _ in files)
    done = 0
    say(f'[{label}] {len(files)} file, {total/1024**3:.2f} GiB')
    last = time.monotonic()
    for number, (item, relative) in enumerate(files, 1):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        # Stream big DLLs instead of one unobservable blocking tree operation.
        with item.open('rb') as src, target.open('wb') as dst:
            for block in iter(lambda: src.read(1024*1024), b''):
                dst.write(block)
                done += len(block)
                if time.monotonic() - last >= 5:
                    say(f'[{label}] {number}/{len(files)} file, {done/1024**3:.2f}/{total/1024**3:.2f} GiB')
                    last = time.monotonic()
        shutil.copystat(item, target)
    say(f'[{label}] completato')


def make_zip(portable, final):
    display_final = Path(final)
    portable, final = filesystem_path(portable), filesystem_path(final)
    partial = final.with_suffix('.zip.partial')
    files = sorted(p for p in portable.rglob('*') if p.is_file())
    total = sum(p.stat().st_size for p in files)
    done = 0
    last = time.monotonic()
    say(f'[ZIP] compressione di {len(files)} file, {total/1024**3:.2f} GiB: {partial}')
    # Level 1 reduces CPU time; ZIP64 supports Blender portable archives above 4 GiB.
    with zipfile.ZipFile(partial, 'x', zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as archive:
        # Preserve the empty version/config directory which makes this a portable
        # preference profile when the archive is extracted on another machine.
        for directory in sorted(p for p in portable.rglob('*') if p.is_dir()):
            archive.write(directory, directory.relative_to(portable).as_posix()+'/')
        for number, path in enumerate(files, 1):
            info = zipfile.ZipInfo.from_file(path, path.relative_to(portable).as_posix())
            info.compress_type = zipfile.ZIP_DEFLATED
            info._compresslevel = 1
            with path.open('rb') as src, archive.open(info, 'w', force_zip64=True) as dst:
                for block in iter(lambda: src.read(1024*1024), b''):
                    dst.write(block)
                    done += len(block)
                    if time.monotonic() - last >= 5:
                        say(f'[ZIP] {number}/{len(files)} file, input {done/1024**3:.2f}/{total/1024**3:.2f} GiB')
                        last = time.monotonic()
    say('[ZIP] verifica CRC di tutti i file...')
    last = time.monotonic()
    with zipfile.ZipFile(partial) as archive:
        if 'blender.exe' not in archive.namelist() or 'BUILD-INFO.json' not in archive.namelist():
            raise RuntimeError('ZIP senza binario o informazioni build; conservo il file .partial')
        entries = archive.infolist()
        for number, info in enumerate(entries, 1):
            with archive.open(info) as src:
                while src.read(1024*1024):
                    if time.monotonic() - last >= 5:
                        say(f'[ZIP] verifica {number}/{len(entries)} voci')
                        last = time.monotonic()
    if final.exists():
        raise FileExistsError(final)
    partial.rename(final)
    say('[ZIP] calcolo SHA256...')
    final.with_suffix('.zip.sha256.txt').write_text(sha256(final, 'SHA256')+'\n', encoding='ascii')
    return display_final


def prepare_patch(git, patch, manifest, source, cache, run):
    pending = []
    incompatible = []
    for number, entry in enumerate(manifest, 1):
        path = entry['path']
        include = '--include='+path
        forward, _ = run_native(git+[include, '--check', str(patch)], run/f'patch-check-{number}.log', 'PATCH_CHECK', accepted=tuple(range(256)))
        if forward == 0:
            pending.append(include)
            continue
        reverse, _ = run_native(git+[include, '--check', '-R', str(patch)], run/f'patch-reverse-check-{number}.log', 'PATCH_REVERSE_CHECK', accepted=tuple(range(256)))
        if reverse != 0:
            incompatible.append(path)
    if incompatible:
        collected = run/'compatibility-sources'
        collected.mkdir()
        for entry in manifest:
            origin = source/entry['path']
            if origin.is_file():
                target = collected/entry['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(origin, target)
        shutil.copy2(cache, collected/'CMakeCache.txt')
        archive = shutil.make_archive(str(run/'compatibility-sources'), 'zip', collected)
        raise RuntimeError(f'Patch incompatibile: nessun sorgente modificato. File: {incompatible}. Invia {archive} e patch-check-*.log per adattarla')
    say(f'[PATCH] {len(pending)} sorgenti da aggiornare; {len(manifest)-len(pending)} gia aggiornati')
    return pending


def validate_resume(git, patch, manifest, source, binary, run):
    saved = json.loads((run/'source-before.json').read_text(encoding='utf-8-sig'))
    if {entry['path'] for entry in saved} != {entry['path'] for entry in manifest}:
        raise RuntimeError('Il backup appartiene a un altro kit: non completo questa cartella')
    log = (run/'build.log').read_text(encoding='utf-8', errors='replace').replace('\\', '/').lower()
    if str(binary).replace('\\', '/').lower() not in log:
        raise RuntimeError('Il log non conferma la generazione di questo blender.exe: usa COMPILA-E-VERIFICA.ps1')
    binary_time = binary.stat().st_mtime_ns
    for entry in manifest:
        path = source/entry['path']
        if path.stat().st_mtime_ns > binary_time:
            raise RuntimeError(f'Sorgente modificato dopo il binario: {path}. Occorre ricompilare')
    run_native(git+['--check', '-R', str(patch)], run/'resume-patch-check.log', 'RESUME_PATCH_CHECK')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--smoke-timeout', type=int, default=180)
    parser.add_argument('--resume-run', type=Path)
    args = parser.parse_args()
    kit = Path(__file__).resolve().parent
    source, build, output = args.source.resolve(), args.build.resolve(), args.output.resolve()
    for base in (source, build):
        if output == base or base in output.parents:
            raise RuntimeError('La cartella di esportazione deve essere esterna a sorgenti e build')
    if os.name != 'nt':
        raise RuntimeError('Questo runner di compilazione richiede Windows/VS2022')
    if not shutil.which('git.exe') or (not args.resume_run and not shutil.which('cmake.exe')):
        raise RuntimeError('Git/CMake non disponibili: usa Developer PowerShell VS2022')
    cache = build/'CMakeCache.txt'
    settings = {}
    for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
        if '=' in line and not line.startswith(('#', '//')):
            key, value = line.split('=', 1)
            settings[key.split(':', 1)[0]] = value
    if Path(settings.get('CMAKE_HOME_DIRECTORY', '')).resolve() != source:
        raise RuntimeError('La build non appartiene ai sorgenti indicati')
    release = build/'bin'/'Release'
    if not (release/'blender.exe').is_file():
        raise RuntimeError('Release precedente mancante: impossibile fare backup')
    previous_launcher = kit/'experiment-tools'
    for name in ('AVVIA-ESPERIMENTO.ps1', 'PROVA-POLYPAINT.py', 'LAYER-DIAGNOSTICS.py'):
        if not (previous_launcher/name).is_file():
            raise RuntimeError(f'File incluso nel kit mancante: {previous_launcher/name}')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6]
    run = args.resume_run.resolve() if args.resume_run else output/('Sculpt-Performance-'+stamp)
    if args.resume_run:
        for base in (source, build):
            if run == base or base in run.parents:
                raise RuntimeError('La cartella da completare deve essere esterna a sorgenti e build')
        for item in ('source-before.json', 'build.log', 'previous-release'):
            if not (run/item).exists():
                raise RuntimeError(f'Cartella di una compilazione precedente incompleta: manca {run/item}')
    else:
        run.mkdir(parents=True, exist_ok=False)
    say(f'CARTELLA RISULTATI E LOG: {run}')
    try:
        manifest = json.loads((kit/'SOURCE-HASHES.json').read_text(encoding='utf-8-sig'))
        precision_source = (source/'source/blender/blenkernel/intern/multires_reshape.cc').read_text(encoding='utf-8-sig')
        if 'multires_preserve_retained_samples' not in precision_source:
            raise RuntimeError('Applica prima la correzione Subdivide-Precisione-V01; nessun sorgente modificato')
        patch = kit/'sculpt-performance.patch'
        git = ['git.exe', '-C', str(source), 'apply', '--ignore-space-change']
        if args.resume_run:
            validate_resume(git, patch, manifest, source, release/'blender.exe', run)
            say('[RIPRESA] Uso la Release gia compilata; backup e sorgenti precedenti conservati.')
        else:
            pending = prepare_patch(git, patch, manifest, source, cache, run)
            before = []
            for entry in manifest:
                path = source/entry['path']
                target = run/'source-before'/entry['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
                before.append({'path':entry['path'], 'sha256':sha256(path)})
            (run/'source-before.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
            shutil.copy2(cache, run/'CMakeCache-before.txt')
            copy_tree(release, run/'previous-release', 'BACKUP_RELEASE')
            if pending:
                run_native(git+pending+[str(patch)], run/'patch-apply.log', 'PATCH_APPLY')
            run_native(['cmake.exe', '--build', str(build), '--config', 'Release', '--target', 'INSTALL', '--parallel', str(args.jobs)], run/'build.log', 'BUILD')
        run_native([sys.executable, '-S', str(kit/'tests'/'validate_export_longpaths.py')], run/'long-path-test.log', 'LONG_PATH_TEST', timeout=90)
        portable = run/'Blender-Sculpt-Performance'
        copy_tree(release, portable, 'COPIA_PORTABLE', portable=True)
        shutil.copy2(kit/'CONTROLLA-BUILD.py', portable/'CONTROLLA-BUILD.py')
        tools = portable/'experiment-tools'
        tools.mkdir(exist_ok=True)
        for name in ('AVVIA-ESPERIMENTO.ps1', 'PROVA-POLYPAINT.py', 'LAYER-DIAGNOSTICS.py'):
            shutil.copy2(previous_launcher/name, tools/name)
        shutil.copy2(kit/'POLYPAINT-BAKE.py', tools/'POLYPAINT-BAKE.py')
        shutil.copy2(kit/'AVVIA-PORTABLE.ps1', portable/'AVVIA-BLENDER.ps1')
        report_path = run/'smoke-check.json'
        env = os.environ.copy()
        env['BLENDER_SCULPT_FIRST_STROKE_ANCHOR'] = '0'
        env['BLENDER_SCULPT_POLYPAINT_MESH_COLOR'] = '1'
        env['BLENDER_SCULPT_POLYPAINT_CPU'] = '1'
        env['BLENDER_SCULPT_SUBDIVIDE_PRESERVE_RETAINED'] = '1'
        env['BLENDER_SCULPT_SUBDIVIDE_COMPACT'] = '1'
        env['BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE'] = '1'
        env['BLENDER_SCULPT_SUBDIVIDE_PROFILE'] = '1'
        run_native([str(portable/'blender.exe'), '--background', '--factory-startup', '--python-exit-code', '1', '--python', str(portable/'CONTROLLA-BUILD.py'), '--', str(report_path), 'Release'], run/'smoke-check.log', 'SMOKE', timeout=args.smoke_timeout, env=env)
        report = json.loads(report_path.read_text(encoding='utf-8'))
        if report.get('status') != 'PASS':
            raise RuntimeError('Smoke check non riuscito: nessuno ZIP finale')
        say('[KEYFRAME_TEST] verifica nativa di geometria, interpolazione, driver e salvataggio su scena piccola...')
        test_directory = run/'keyframe-test'
        run_native([str(portable/'blender.exe'), '--background', '--factory-startup', '--python-exit-code', '1', '--python', str(kit/'VERIFICA-KEYFRAME.py'), '--', str(test_directory)], run/'keyframe-test.log', 'KEYFRAME_TEST', timeout=max(args.smoke_timeout, 300), env=env)
        keyframe_report = json.loads((test_directory/'strength-keyframes-report.json').read_text(encoding='utf-8'))
        if keyframe_report.get('status') != 'PASS':
            raise RuntimeError('Controllo keyframe non riuscito: nessuno ZIP finale; conserva keyframe-test.log')
        report['strength_keyframes_test'] = keyframe_report
        say('[PRECISION_TEST] controllo della precisione e Delete Higher Object/Sculpt...')
        precision_directory = run/'precision-test'
        run_native([str(portable/'blender.exe'), '--background', '--factory-startup', '--python-exit-code', '1', '--python', str(kit/'VERIFICA-PRECISIONE.py'), '--', str(precision_directory)], run/'precision-test.log', 'PRECISION_TEST', timeout=max(args.smoke_timeout, 300), env=env)
        delete_report = json.loads((precision_directory/'delete-higher-report.json').read_text(encoding='utf-8'))
        if delete_report.get('status') != 'PASS':
            raise RuntimeError('Precisione non verificata: nessuno ZIP finale')
        report['delete_higher_test'] = delete_report
        native_runs = {}
        for variant,flag in (('legacy','0'),('compact','1')):
            variant_env = dict(env)
            variant_env['BLENDER_SCULPT_SUBDIVIDE_COMPACT'] = flag
            directory = run/('ab-'+variant)
            run_native([str(portable/'blender.exe'), '--background', '--factory-startup', '--python-exit-code', '1', '--python', str(kit/'VERIFICA-SUBDIVIDE-AB.py'), '--', str(directory)], run/('ab-'+variant+'.log'), 'AB_'+variant.upper(), timeout=max(args.smoke_timeout, 600), env=variant_env)
            native_runs[variant] = json.loads((directory/'subdivide-ab.json').read_text(encoding='utf-8'))
        from confronta_ab import compare
        comparison = compare(native_runs['legacy'],native_runs['compact'])
        (run/'subdivide-ab-comparison.json').write_text(json.dumps(comparison,indent=2),encoding='utf-8')
        report['subdivision_ab'] = comparison
        say('[SUBDIVIDE_AB] PASS geometria al livello mantenuto/nuovo, layer isolati, colore, metadati e riapertura')
        version_dir = portable/report['version_major_minor']
        if not version_dir.is_dir():
            raise RuntimeError('Risorse della versione mancanti')
        (version_dir/'config').mkdir(exist_ok=True)
        after = []
        for entry in manifest:
            path = source/entry['path']
            target = run/'source-after'/entry['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            after.append({'path':entry['path'], 'sha256':sha256(path)})
        info = {'requested_configuration':'Release', 'incremental_build':True, 'resumed_existing_build':bool(args.resume_run), 'source':str(source), 'build':str(build), 'source_files':after, 'patch_sha256':sha256(patch), 'binary_sha256':sha256(portable/'blender.exe'), 'smoke_check':report, 'paint_undo_redo_validated':False, 'delete_higher_undo_redo_validated':False, 'delete_higher_small_scene_validated':True, 'strength_keyframes_object_mode_validated':True, 'strength_keyframes_render_validated':False, 'layer_subdivision_retained_geometry_validated':True, 'subdivision_legacy_equivalence_validated':True, 'undo_strokes_tested':False, 'undo_history_changes':False, 'experimental_profile':'Existing optimizations + retained precision + compact subdivision; Undo compression diagnostics only'}
        (portable/'BUILD-INFO.json').write_text(json.dumps(info, indent=2), encoding='utf-8')
        (kit/'experiment-paths.json').write_text(json.dumps({'SourceDir':str(source), 'BuildDir':str(build), 'BlenderExe':str(release/'blender.exe'), 'Logs':str(run)}, indent=2), encoding='utf-8')
        final_path = run/(run.name+'.zip')
        if final_path.exists() or final_path.with_suffix('.zip.partial').exists():
            final_path = run/(run.name+'-completed-'+stamp+'.zip')
        final = make_zip(portable, final_path)
        say(f'COMPLETATO\nPORTABLE: {portable}\nZIP: {final}\nAVVIO: {portable / "AVVIA-BLENDER.ps1"}')
        say('Non avvio automaticamente Blender. Scena di prova: '+str(test_directory/'Strength-Keyframes-Test.blend'))
    except BaseException:
        try:
            filesystem_path(run/'operation-error.log').write_text(traceback.format_exc(), encoding='utf-8')
        except OSError:
            say('[LOG] Non posso scrivere operation-error.log; conserva anche il testo della console.')
        say(f'OPERAZIONE NON COMPLETATA. Backup, binari e log conservati in: {run}')
        raise


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        say(f'ERRORE: {exc}')
        sys.exit(1)
