import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import types

kit = Path('Sculpt-Bake-Visible-Hidden-V01').resolve()
sys.path.insert(0, str(kit))
spec = importlib.util.spec_from_file_location('native_driver', kit / 'PROVA.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
helper = module.helper


def environment(root):
    source, build, run = root / 'source', root / 'build', root / 'run'
    run.mkdir()
    (build / 'bin/Release').mkdir(parents=True)
    (build / 'bin/Release/blender.exe').write_bytes(b'original executable')
    rows = json.loads((kit / 'PATCH.json').read_text())
    for row in rows:
        if row['before'] is None:
            continue
        target = source / row['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        prior = Path('Sculpt-Subdivide-Shared-Work-V03/payload', row['file'])
        if not prior.exists():
            prior = Path('working/release_current/blender') / row['path']
        text = prior.read_text()
        target.write_bytes(text.replace('\n', '\r\n').encode())
    return source, build, run, rows


for initially_changed in (False, True):
    with tempfile.TemporaryDirectory() as tmp:
        source, build, run, rows = environment(Path(tmp))
        original = {r['path']: (source / r['path']).read_bytes() for r in rows if r['before'] is not None}
        helper.KIT = kit
        with contextlib.redirect_stdout(io.StringIO()):
            helper.apply(source, build, run)
            if not initially_changed:
                # Failure/interruption before replacement leaves original bytes.
                for path, content in original.items():
                    (source / path).write_bytes(content)
            (build / 'bin/Release/blender.exe').write_bytes(b'compiled diagnostic')
            module.restore_original(run)
        assert all((source / path).read_bytes() == content for path, content in original.items())
        assert (build / 'bin/Release/blender.exe').read_bytes() == b'original executable'
        assert json.loads((run / 'RIPRISTINO.json').read_text())['status'] == 'RESTORED'
        assert not (source / 'source/blender/blenkernel/intern/multires_subdivide_support_v03.hh').exists()
print('byte_exact_restore_before_or_after_source_replacement PASS')

with tempfile.TemporaryDirectory() as tmp:
    source, build, run, rows = environment(Path(tmp))
    helper.KIT = kit
    target = source / rows[1]['path']
    target.write_text('unrecognized source')
    original_exe = (build / 'bin/Release/blender.exe').read_bytes()
    try:
        helper.apply(source, build, run)
        raise AssertionError('preflight accepted modified guard')
    except RuntimeError:
        pass
    assert (build / 'bin/Release/blender.exe').read_bytes() == original_exe
    assert not (run / 'backup').exists()
print('all_preflight_guards_before_backup_and_writes PASS')

with tempfile.TemporaryDirectory() as tmp:
    source, build, run, rows = environment(Path(tmp))
    helper.KIT = kit
    with contextlib.redirect_stdout(io.StringIO()):
        helper.apply(source, build, run)
    target = source / rows[0]['path']
    target.write_text('concurrent user change')
    try:
        module.restore_original(run)
        raise AssertionError('restore accepted concurrent edit')
    except RuntimeError:
        pass
    assert target.read_text() == 'concurrent user change'
print('concurrent_source_edit_blocks_overwrite PASS')

# Exercise main's actual build-failure finally path, not just restore() directly.
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source, build, unused, rows = environment(root)
    testkit = root / 'kit'
    testkit.mkdir()
    shutil.copy2(kit / 'PATCH.json', testkit / 'PATCH.json')
    shutil.copytree(kit / 'payload', testkit / 'payload')
    cmake = root / 'cmake.exe'
    cmake.write_bytes(b'fake cmake')
    (build / 'CMakeCache.txt').write_text('CMAKE_COMMAND:FILEPATH=' + str(cmake) + '\nCMAKE_HOME_DIRECTORY:PATH=' + str(source) + '\nCMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n')
    original = {r['path']: (source / r['path']).read_bytes() for r in rows if r['before'] is not None}
    module.KIT = helper.KIT = testkit
    module.CURRENT = helper.sha(build / 'bin/Release/blender.exe')
    module.os = types.SimpleNamespace(name='nt')
    module.integrity = lambda: None
    module.original_exe = lambda: root / 'reference.exe'
    helper.idle = lambda: None
    def command(args, log, **kwargs):
        log.write_text('mock command')
        if '--build' in args and build in args:
            (build / 'bin/Release/blender.exe').write_bytes(b'partially built candidate')
            raise RuntimeError('injected native build failure')
        if '--target' in args:
            path = Path(args[args.index('--build') + 1]) / 'Release/native_replay.exe'
            path.parent.mkdir(parents=True)
            path.write_bytes(b'fake replay')
    helper.command = command
    with contextlib.redirect_stdout(io.StringIO()):
        try:
            module.main(types.SimpleNamespace(source=source, build=build, parallel=4, restore=None, launch=False, rotation=False))
            raise AssertionError('main accepted failed build')
        except RuntimeError as exc:
            assert 'injected native build failure' in str(exc)
    assert all((source / path).read_bytes() == value for path, value in original.items())
    assert (build / 'bin/Release/blender.exe').read_bytes() == b'original executable'
    run = next(testkit.glob('prova-*'))
    assert (run / 'LOG-DA-INVIARE.zip').is_file()
    assert json.loads((run / 'RISULTATO.json').read_text())['original_restored'] is True
print('native_build_failure_restores_original_and_collects_logs PASS')
