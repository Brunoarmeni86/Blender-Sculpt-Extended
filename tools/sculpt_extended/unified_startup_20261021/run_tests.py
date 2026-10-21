"""Check the installed native build without external feature setup."""
import argparse
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--blender', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
test = Path(__file__).with_name('test_native.py')
base_env = {k: v for k, v in os.environ.items() if not k.startswith(('BLENDER_', 'FACESETS_'))}
for mode in ('normal', 'override', 'optout'):
    env = base_env.copy()
    if mode == 'override':
        env.update(BLENDER_SCULPT_LAYER_CACHE_MB='256', BLENDER_SCULPT_WEIGHT_MULTI_CACHE='0')
    elif mode == 'optout':
        env['BLENDER_SCULPT_EXTENDED_NATIVE'] = '0'
    with (args.output / f'{mode}.log').open('w', encoding='utf-8') as log:
        subprocess.run([
            str(args.blender.resolve()), '--background', '--factory-startup',
            '--python-exit-code', '1', '--python', str(test), '--', mode,
            str(args.output.resolve()),
        ], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    result = json.loads((args.output / f'test-{mode}.json').read_text())
    assert result['status'] == 'PASS'
    print(f'{mode}: PASS', flush=True)
