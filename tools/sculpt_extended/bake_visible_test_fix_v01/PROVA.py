"""Repeat focused bake/UI tests against the existing native binary; no build writes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import zipfile
import BUILD_HELPER as helper
KIT = Path(__file__).resolve().parent
EXPECTED = 'da235eb28ff27873414ca095104bf325e11a04413f6ac63cae36ba76c8cd9f5d'
ORIGINAL_KIT = Path(r'C:\BlenderDev\Sculpt-Bake-Visible-Hidden-V01')


def environment():
    env = helper.environment('unchanged_candidate')
    env.update(BLENDER_SCULPT_SUBDIVIDE_REGIONAL_V03='1',
               BLENDER_SCULPT_SUBDIVIDE_TOPOLOGY_V03='0',
               BLENDER_SCULPT_SUBDIVIDE_BASE_PHASE_V03='0',
               BLENDER_MULTIRES_DETAIL_TRANSPORT='affine')
    env.pop('BLENDER_SCULPT_REGION_CAPTURE_V02', None)
    return env


def candidate():
    run = Path((ORIGINAL_KIT / 'ULTIMA.txt').read_text().strip())
    exe = run / 'Blender-Sculpt-Bake-Visible-Hidden-V01/blender.exe'
    if not exe.is_file() or helper.sha(exe) != EXPECTED:
        raise RuntimeError('Portable Bake Visible diversa da quella del log ricevuto: ' + str(exe))
    return exe


def collect(run):
    with zipfile.ZipFile(run / 'LOG-DA-INVIARE.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for p in run.rglob('*'):
            if p.is_file() and p.suffix in ('.json', '.log'):
                z.write(p, p.relative_to(run))
    print('LOG DA INVIARE: ' + str(run / 'LOG-DA-INVIARE.zip'), flush=True)


def main(launch):
    if os.name != 'nt':
        raise RuntimeError('Eseguire sul Windows della build')
    helper.idle()
    exe = candidate()
    integrity = json.loads((KIT / 'INTEGRITA.json').read_text())
    for name, expected in integrity.items():
        if helper.sha(KIT / name) != expected:
            raise RuntimeError('Kit modificato: ' + name)
    if launch:
        run = Path((KIT / 'ULTIMA.txt').read_text().strip())
        data = json.loads((run / 'RISULTATO.json').read_text())
        if data.get('status') != 'PASS_BAKE_VISIBLE_HIDDEN' or data.get('candidate_sha256') != EXPECTED:
            raise RuntimeError('La prova completa non passa')
        subprocess.Popen([str(exe)], env=environment())
        return
    run = KIT / ('prova-' + time.strftime('%Y%m%d-%H%M%S'))
    run.mkdir()
    (KIT / 'ULTIMA.txt').write_text(str(run))
    state = dict(status='RUNNING', candidate_sha256=EXPECTED, exe=str(exe),
                 source_changes=False, compilation=False, results={})
    try:
        for case in ['HR', 'L0']:
            folder = run / case
            folder.mkdir()
            state['results'][case] = {}
            for action in ['seed', 'exercise', 'reopen']:
                print(case + ': ' + action + ' (nessuna ricompilazione)', flush=True)
                env = environment()
                env.update(SUBDIV_TEST_ROOT=str(folder), SUBDIV_TEST_PHASE=action,
                           SUBDIV_SEED=str(folder / 'seed.blend'), SUBDIV_REFERENCE='',
                           BAKE_L0='1' if case == 'L0' else '0')
                helper.command([exe, '--factory-startup', '--no-window-focus', '--python', KIT / 'WORKER.py'],
                               folder / (action + '.log'), env=env, timeout=600)
                result = json.loads((folder / (action + '.json')).read_text())
                state['results'][case][action] = result
                if not result.get('status', '').startswith('PASS'):
                    raise RuntimeError(result.get('error', case + ' ' + action + ' incompleto'))
        state['status'] = 'PASS_BAKE_VISIBLE_HIDDEN'
        print('PASS_BAKE_VISIBLE_HIDDEN', flush=True)
    except BaseException as exc:
        state.update(status='REVIEW_REQUIRED', error=str(exc))
        raise
    finally:
        (run / 'RISULTATO.json').write_text(json.dumps(state, indent=2))
        collect(run)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--launch', action='store_true')
    try:
        main(p.parse_args().launch)
    except Exception as exc:
        print('ERRORE: ' + str(exc), flush=True)
        raise SystemExit(1)
