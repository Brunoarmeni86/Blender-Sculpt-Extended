"""Real stdlib copy/ZIP checks; run unchanged on Windows for native path verification."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import zipfile

kit=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('export_runner',kit/'compila_e_zip.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
for original,expected in (
    (r'C:\folder\file.txt',r'\\?\C:\folder\file.txt'),
    ('C:/folder/../folder/file.txt',r'\\?\C:\folder\file.txt'),
    (r'\\server\share\folder\file.txt',r'\\?\UNC\server\share\folder\file.txt'),
    (r'\\?\C:\folder\file.txt',r'\\?\C:\folder\file.txt'),
    (r'\\?\UNC\server\share\folder\file.txt',r'\\?\UNC\server\share\folder\file.txt'),
):
    assert runner.windows_extended_path(original)==expected
try:
    runner.windows_extended_path('relative/file.txt')
except ValueError:
    pass
else:
    raise AssertionError('absolute filesystem path required')
with tempfile.TemporaryDirectory() as folder:
    base=Path(folder);source=base/'release';source.mkdir()
    files={'blender.exe':b'test binary bytes, not a native Blender',
           'BUILD-INFO.json':b'{"test":true}',
           '5.2/python/lib/site-packages/pkg_resources/tests/data/my-test-package_unpacked-egg/my_test_package-1.0-py3.7.egg/EGG-INFO/dependency_links.txt':b'\n',
           'resources/payload with spaces.bin':bytes(range(256))*8192,
           'resources/unicode-\u00e0.txt':'unicode bytes \u00e8'.encode(),
           '5.2/config/userpref.blend':b'user preferences'}
    for relative,data in files.items():
        target=runner.filesystem_path(source/relative);target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
    deep=base/('a'*80)/('b'*80)/('c'*80)/('d'*80)
    destination=deep/'portable'
    assert len(str(destination))>260
    runner.copy_tree(source,destination,'LONGPATH_PORTABLE',portable=True)
    copied=runner.filesystem_path(destination)
    for relative,data in files.items():
        if 'config/' in relative:
            assert not (copied/relative).exists()
        else:
            assert (copied/relative).read_bytes()==data
    (copied/'5.2/config').mkdir()
    archive=runner.make_zip(destination,deep/'portable.zip')
    with zipfile.ZipFile(runner.filesystem_path(archive)) as z:
        assert z.testzip() is None and '5.2/config/' in z.namelist()
        for relative,data in files.items():
            if 'config/' not in relative:
                assert z.read(relative)==data
    checksum=runner.filesystem_path(archive.with_suffix('.zip.sha256.txt')).read_text().strip()
    assert checksum==runner.sha256(archive)
    assert not runner.filesystem_path(archive.with_suffix('.zip.partial')).exists()
    backup=deep/'backup'
    runner.copy_tree(source,backup,'LONGPATH_BACKUP')
    assert (runner.filesystem_path(backup)/'5.2/config/userpref.blend').read_bytes()==b'user preferences'
    # Remove only this test's temporary deep tree through the same extended API;
    # TemporaryDirectory's ordinary cleanup need not understand long Win32 paths.
    shutil.rmtree(runner.filesystem_path(deep))
    # deep is only the last (d...) directory; its long a/b/c ancestors also
    # belong to this temporary test and must be removed via extended paths.
    shutil.rmtree(runner.filesystem_path(base/('a'*80)))
print('PASS drive/UNC/extended path conversion + real deep-path copy, backup, ZIP bytes/CRC/SHA and profile isolation')
