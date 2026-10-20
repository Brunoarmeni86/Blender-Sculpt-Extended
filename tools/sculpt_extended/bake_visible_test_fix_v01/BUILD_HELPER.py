"""Build and compare scoped topology/base-phase reuse against verified Subdivide V02."""
import argparse
import csv
import hashlib
import io
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time
import zipfile

KIT = Path(__file__).resolve().parent


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(4 * 1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def text_sha(path):
    return hashlib.sha256(path.read_text(encoding='utf-8-sig').encode()).hexdigest()


def command(args, log, env=None, timeout=None):
    print('ESEGUO: ' + subprocess.list2cmdline([str(a) for a in args]), flush=True)
    with log.open('w', encoding='utf-8') as stream:
        proc = subprocess.Popen([str(a) for a in args], stdout=stream,
            stderr=subprocess.STDOUT, env=env)
        start = time.monotonic()
        try:
            while proc.poll() is None:
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    print('  %ds | %s' % (time.monotonic() - start, log), flush=True)
                if timeout and proc.poll() is None and time.monotonic() - start > timeout:
                    raise RuntimeError('Timeout: ' + str(log))
        except BaseException:
            if proc.poll() is None:
                subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            raise
    if proc.returncode:
        raise RuntimeError('Comando fallito (%s): %s' % (proc.returncode, log))


def idle():
    output = subprocess.check_output(['tasklist', '/FO', 'CSV', '/NH'], text=True, errors='replace')
    processes = {row[0].lower() for row in csv.reader(io.StringIO(output)) if row}
    active = processes.intersection({'blender.exe', 'cmake.exe', 'ninja.exe', 'cl.exe', 'link.exe'})
    if active:
        raise RuntimeError('Chiudi Blender o attendi la compilazione: ' + ', '.join(sorted(active)))


def apply(source, build, run):
    rows = json.loads((KIT / 'PATCH.json').read_text())
    changes = []
    for row in rows:
        payload = KIT / 'payload' / row['file']
        if text_sha(payload) != row['after']:
            raise RuntimeError('Payload non valido: ' + row['file'])
        target = source / row['path']
        current = text_sha(target) if target.exists() else None
        if current not in (row['before'], row['after']):
            raise RuntimeError('Sorgente differente dalla base verificata: ' + row['path'])
        if current != row['after']:
            changes.append(row)
    backup = run / 'backup'
    backup.mkdir()
    exe = build / 'bin/Release/blender.exe'
    shutil.copy2(exe, backup / 'blender.exe')
    if sha(exe) != sha(backup / 'blender.exe'):
        raise RuntimeError('Backup exe non valido')
    records = []
    for row in changes:
        target = source / row['path']
        saved = backup / row['path']
        if target.exists():
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, saved)
            if sha(target) != sha(saved):
                raise RuntimeError('Backup sorgente non valido: ' + row['path'])
        records.append(dict(path=row['path'], existed=target.exists(),
            before_bytes_sha256=sha(target) if target.exists() else None,
            after_text_sha256=row['after']))
    (backup / 'RESTORE.json').write_text(json.dumps(dict(source=str(source), build=str(build),
        exe_sha256=sha(backup / 'blender.exe'), files=records), indent=2))
    # All preflight and backups are complete before the first source write.
    for row in changes:
        target = source / row['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + '.regional-v02.tmp')
        if temporary.exists():
            raise RuntimeError('File temporaneo gia presente: ' + str(temporary))
        temporary.write_text((KIT / 'payload' / row['file']).read_text(), encoding='utf-8', newline='\n')
        os.replace(temporary, target)
    print('Backup: ' + str(backup), flush=True)


def restore(backup):
    data = json.loads((backup / 'RESTORE.json').read_text())
    source = Path(data['source'])
    for row in data['files']:
        target = source / row['path']
        if not target.is_file() or text_sha(target) != row['after_text_sha256']:
            raise RuntimeError('Sorgente cambiato dopo la patch: ' + row['path'])
        if row['existed'] and sha(backup / row['path']) != row['before_bytes_sha256']:
            raise RuntimeError('Backup sorgente non valido: ' + row['path'])
    if sha(backup / 'blender.exe') != data['exe_sha256']:
        raise RuntimeError('Backup exe non valido')
    for row in data['files']:
        target = source / row['path']
        if row['existed']:
            shutil.copy2(backup / row['path'], target)
            target.touch()  # Invalidate objects compiled against the V03 context layout.
        else:
            target.unlink()
    shutil.copy2(backup / 'blender.exe', Path(data['build']) / 'bin/Release/blender.exe')
    print('Ripristinati sorgenti modificati ed exe precedente. La portable sperimentale resta separata.')



STAGE = 'Blender-Sculpt-Extended-Subdivide-V03'
EXPECTED_BASE = '97a3b5c6867ba33a47241561f9d57de8ca7c9c36423e7fce1d13c7a5abb656c0'
COMMON = {
    'BLENDER_SCULPT_EXTENDED_NATIVE': '1',
    'BLENDER_LAYER_RESP_TRACE': '1',
    'BLENDER_SCULPT_LAYER_REGION_V01': '1',
    'BLENDER_SCULPT_WEIGHT_HOT_SKIP_STITCH': '0',
    'BLENDER_SCULPT_WEIGHT_MULTI_CACHE': '0',
    'BLENDER_SCULPT_WEIGHT_HOT_DELTA_CACHE': '1',
    'BLENDER_SCULPT_VISIBLE_INCREMENTAL': '1',
    'BLENDER_SCULPT_VISIBLE_DIRECT_DELTA': '1',
    'BLENDER_SCULPT_LOCAL_COMMIT': '1',
    'BLENDER_SCULPT_COMMIT_BATCHED_FRAME_BUILD': '1',
    'BLENDER_SCULPT_COMMIT_KEEP_CCG': '0',
    'BLENDER_SCULPT_LAYER_READONLY_WEIGHT': '1',
    'BLENDER_SCULPT_LAYER_SUPPORT_CACHE': '1',
    'BLENDER_SCULPT_FAST_TANGENT_SOLVE': '1',
    'BLENDER_SCULPT_COMMIT_FRAME_CACHE': '1',
    'BLENDER_SCULPT_LAYER_CACHE_MB': '256',
    'BLENDER_SCULPT_COMMIT_FRAME_CACHE_GRIDS': '8192',
    'BLENDER_SCULPT_SUBDIVIDE_PROFILE': '1',
    'BLENDER_SCULPT_SUBDIVIDE_MOVE_SCRATCH_V02': '1',
    'BLENDER_SCULPT_SUBDIVIDE_COARSE_REUSE_V02': '1',
    'BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE': '0',
    'BLENDER_SCULPT_SUBDIVIDE_PRESERVE_RETAINED': '0',
    'BLENDER_SCULPT_SUBDIVIDE_WORKERS_V01': '4',
}
PROFILES = {name: (1, 1, 1) for name in
            ['reference', 'unchanged_candidate', 'topology_only', 'zero_budget', 'optimized']}
V03 = {
    'reference': (0, 0, 256), 'unchanged_candidate': (0, 0, 256),
    'topology_only': (1, 0, 256), 'zero_budget': (1, 1, 0),
    'optimized': (1, 1, 256),
}



def environment(profile):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('BLENDER_SCULPT_', 'BLENDER_FACESETS_',
                                'BLENDER_MULTIRES_FACESETS_', 'FACESETS_RC1_'))
           and k != 'BLENDER_SUBDIV_PROFILE_LOG'}
    env.update(COMMON)
    for name, value in zip(['BLENDER_SCULPT_SUBDIVIDE_COMPACT',
                           'BLENDER_SCULPT_SUBDIVIDE_PARALLEL_V01',
                           'BLENDER_SCULPT_SUBDIVIDE_ZERO_V01'], PROFILES[profile]):
        env[name] = str(value)
    for name, value in zip(['BLENDER_SCULPT_SUBDIVIDE_TOPOLOGY_V03',
                            'BLENDER_SCULPT_SUBDIVIDE_BASE_PHASE_V03',
                            'BLENDER_SCULPT_SUBDIVIDE_BASE_CACHE_MB_V03'], V03[profile]):
        env[name] = str(value)
    return env


def reference_exe(explicit=None):
    if explicit:
        exe = explicit.resolve()
    else:
        location = Path(r'C:\BlenderDev\Sculpt-Subdivide-Optimize-V02\ULTIMA.txt')
        run = Path(location.read_text().strip())
        exe = run / 'Blender-Sculpt-Extended-Subdivide-V02/blender.exe'
    if not exe.is_file() or sha(exe) != EXPECTED_BASE:
        raise RuntimeError('Riferimento differente dalla build verificata Subdivide V02: ' + str(exe))
    return exe


def phase(exe, run, profile, name, root, reference=None):
    root.mkdir(exist_ok=True)
    env = environment(profile)
    env.update(SUBDIV_TEST_ROOT=str(root), SUBDIV_TEST_PHASE=name,
               SUBDIV_SEED=str(run / 'test/seed/seed.blend'),
               SUBDIV_REFERENCE=str(reference) if reference else '')
    result_file = root / (name + '.json')
    result_file.unlink(missing_ok=True)
    command([exe, '--factory-startup', '--no-window-focus', '--python', KIT / 'WORKER.py'],
            root / (name + '.log'), env=env, timeout=900)
    result = json.loads(result_file.read_text())
    if not result.get('status', '').startswith('PASS'):
        raise RuntimeError(result.get('error', 'Fase incompleta: ' + str(result_file)))
    return result


def audits(log):
    result = {}
    for line in log.splitlines():
        if '[MULTIRES_BASE_AUDIT] ' in line:
            value = json.loads(line.split('[MULTIRES_BASE_AUDIT] ', 1)[1])
            if value['invalid']:
                raise RuntimeError('Audit nativo non valido: ' + value['label'])
            result[value['label']] = value
    if not result:
        raise RuntimeError('Audit nativi assenti.')
    return result


def verify(profile, log, reference_audits):
    own = audits(log)
    if reference_audits:
        if own.keys() != reference_audits.keys():
            raise RuntimeError('Copertura audit diversa.')
        for label, state in own.items():
            for key in ['cage_hash', 'layers', 'active', 'sculpt', 'top', 'visible']:
                if state[key] != reference_audits[label][key]:
                    raise RuntimeError('Cage/metadati differenti: %s %s' % (label, key))
        if own['initial']['native_hash'] != reference_audits['initial']['native_hash']:
            raise RuntimeError('Base iniziale differente.')
    # Unchanged-binary controls demonstrated cross-process native-hash
    # variation at float rounding scale. Preserve exact within-run invariants;
    # numerical surfaces and independent domains retain their original checks.
    for group in [('L4', 'redo_L4'), ('L5', 'base_L5', 'A_L5', 'zero_L5',
                  'negative_L5', 'B_L5', 'both_L5', 'post_stroke_L5', 'low_L1', 'saved')]:
        if len({own[label]['native_hash'] for label in group}) != 1:
            raise RuntimeError('Base modificata durante operazioni sui layer.')
    if own['undo_L3']['native_hash'] != own['initial']['native_hash']:
        raise RuntimeError('Undo non ripristina la base.')
    compact = re.findall(r'\[SUBDIVIDE_COMPACT\] success=(\d+) layers=(\d+) empty=(\d+) active=(\d+)', log)
    if len(compact) != 2 or any(tuple(map(int, row)) != (1, 4, 2, 2) for row in compact):
        raise RuntimeError('Percorso compatto non esercitato: ' + str(compact))
    shared = re.findall(r'\[SUBDIVIDE_SHARED_V03\] topology=(\w+) base=(\w+) retained_mib=([\d.]+)', log)
    if V03[profile][0]:
        base = 'BUILD' if profile == 'optimized' else 'BYPASS'
        hit = 'HIT' if profile == 'optimized' else 'BYPASS'
        expected = [('BUILD', base), ('HIT', hit), ('HIT', hit)] * 2
        if [(top, detail) for top, detail, _ in shared] != expected:
            raise RuntimeError('Riuso delle fasi non esercitato come atteso: ' + str(shared))
        if any(float(mb) > V03[profile][2] for _, _, mb in shared):
            raise RuntimeError('Budget della cache base superato.')
    elif shared:
        raise RuntimeError('Riuso attivo nel controllo senza flag.')
    return own


def test(run, old_exe):
    candidate = run / STAGE / 'blender.exe'
    root = run / 'test'
    root.mkdir(exist_ok=True)
    report = dict(status='RUNNING', exe_sha256=sha(candidate), reference_exe=str(old_exe),
                  reference_sha256=sha(old_exe), profiles={},
                  scope='Native Subdivide 3->4->5 on a shared small fixture; visible/hidden and empty layers; zero/negative strength, Undo/Redo, subsequent Draw, level changes, save/reopen, native-only control. Geometry tolerance 2e-6 relative scale; independent domains and native cage/domains exact; cross-run native hashes recorded separately; within-run native base exact. Dense/sparse layer hashes may differ across profiles. One operator timing per level is diagnostic, not a benchmark.')
    reference_audits = None
    try:
        print('  [1/6] Creazione scena comune sulla V02...', flush=True)
        phase(old_exe, run, 'reference', 'seed', root / 'seed')
        for index, profile in enumerate(PROFILES, 2):
            print('  [%d/6] %s: Subdivide, controlli e riapertura...' % (index, profile), flush=True)
            exe = old_exe if profile == 'reference' else candidate
            ref = None if profile == 'reference' else root / 'reference'
            folder = root / profile
            try:
                data = phase(exe, run, profile, 'exercise', folder, ref)
                reopened = phase(exe, run, profile, 'reopen', folder, ref)
                log = (folder / 'exercise.log').read_text(encoding='utf-8', errors='replace')
                own = verify(profile, log, reference_audits)
                reopen_log = (folder / 'reopen.log').read_text(encoding='utf-8', errors='replace')
                if audits(reopen_log)['reopen']['native_hash'] != own['saved']['native_hash']:
                    raise RuntimeError('Riapertura modifica la base nativa.')
                if profile == 'reference':
                    reference_audits = own
                phases = []
                for line in log.splitlines():
                    if '[SUBDIV_PHASE]' in line:
                        phases.append(line[line.index('[SUBDIV_PHASE]'):])
                zero = re.findall(r'\[SUBDIVIDE_ZERO_V01\].*?released=(\d+) payload_saved_mib=([\d.]+)', log)
                report['profiles'][profile] = dict(status='PASS', flags_v03=V03[profile],
                    timings_ms=data['timings_ms'], max_errors=data['max_errors'],
                    hash_differences=data.get('representation_hash_differences', {}),
                    reopen_status=reopened['status'], zero_payloads=zero, subdivision_phases=phases)
                print('    PASS ' + profile, flush=True)
            except Exception as exc:
                report['profiles'][profile] = dict(status='FAIL', error=str(exc), flags_v03=V03[profile])
                print('    FAIL ' + profile + ': ' + str(exc), flush=True)
                if profile == 'reference':
                    raise RuntimeError('Il riferimento non passa; prova fermata.') from exc
            finally:
                (run / 'RISULTATO.json').write_text(json.dumps(report, indent=2))
        report['status'] = 'PASS_ALL' if all(p['status'] == 'PASS' for p in report['profiles'].values()) else 'REVIEW_REQUIRED'
    except Exception as exc:
        report['status'] = 'FAIL'
        report['error'] = str(exc)
        raise
    finally:
        (run / 'RISULTATO.json').write_text(json.dumps(report, indent=2))
    print(report['status'] + ': invia LOG-DA-INVIARE.zip prima di usare il profilo su progetti reali.', flush=True)


def collect_logs(run):
    paths = [run / 'build.log', run / 'install.log', run / 'RISULTATO.json',
             run / 'backup/RESTORE.json', run / 'PATCH-APPLICATA.json']
    if (run / 'test').exists():
        paths += [p for p in (run / 'test').rglob('*') if p.suffix in {'.log', '.json'}]
    with zipfile.ZipFile(run / 'LOG-DA-INVIARE.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in paths:
            if path.is_file():
                archive.write(path, path.relative_to(run))
    print('LOG DA INVIARE: ' + str(run / 'LOG-DA-INVIARE.zip'), flush=True)


if __name__ == '__main__':
    raise SystemExit('Usare PROVA.py: questo file e solo un modulo di supporto.')
