"""Reverse the exact per-run prepared patch; keep all verified multi-attribute changes."""
import argparse,datetime,json,os,shutil,traceback,uuid
from pathlib import Path
from build_helpers import copy_tree,run_native,say,sha256
from compila_e_zip import settings
from prepare_patch import sha
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True);p.add_argument('--build',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--jobs',type=int,default=4)
p.add_argument('--run',type=Path,required=True)
a=p.parse_args();kit=Path(__file__).resolve().parent
if os.name!='nt':raise RuntimeError('Richiede Windows')
source,build,output,applied=a.source.resolve(),a.build.resolve(),a.output.resolve(),a.run.resolve()
cache,cmake=settings(build)
if Path(cache.get('CMAKE_HOME_DIRECTORY','')).resolve()!=source:raise RuntimeError('Build/sorgenti diversi')
if output==source or source in output.parents or output==build or build in output.parents:raise RuntimeError('Output non valido')
manifest=json.loads((applied/'source-manifest.json').read_text());after=json.loads((applied/'source-after.json').read_text())
for e in after:
    if sha256(source/e['path'])!=e['sha256']:raise RuntimeError('Sorgenti modificati dopo la patch: '+e['path'])
run=output/('PaintPrepared-Ripristino-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6]);run.mkdir(parents=True)
say('LOG E BACKUP RIPRISTINO: '+str(run))
try:
    patch=applied/'applied.patch';git=['git.exe','-C',str(source),'apply','--ignore-space-change']
    run_native(git+['--check','-R',str(patch)],run/'reverse-check.log','REVERSE_CHECK')
    for e in manifest:
        target=run/'source-before'/e['path'];target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source/e['path'],target)
    copy_tree(build/'bin/Release',run/'previous-release','BACKUP_RELEASE')
    run_native(git+['-R',str(patch)],run/'reverse-apply.log','REVERSE_APPLY')
    for e in manifest:
        path=source/e['path']
        if e['before_sha256'] is None:
            if path.exists():raise RuntimeError('Header non rimosso dal ripristino')
        elif sha(path.read_bytes())!=e['before_sha256']:raise RuntimeError('Sorgente ripristinato diverso: '+e['path'])
    run_native([str(cmake),'--build',str(build),'--config','Release','--target','INSTALL','--parallel',str(a.jobs)],run/'build.log','BUILD')
    run_native([str(build/'bin/Release/blender.exe'),'--background','--factory-startup','--python-exit-code','1','--python',str(kit/'CONTROLLA-BUILD.py'),'--',str(run/'smoke.json'),'Release'],run/'smoke.log','SMOKE',timeout=180)
    if json.loads((run/'smoke.json').read_text()).get('status')!='PASS':raise RuntimeError('Smoke fallito')
    say('RIPRISTINO COMPLETATO; Conform/Undo, bake, lazy e multi-attributo conservati.')
except BaseException:
    (run/'operation-error.log').write_text(traceback.format_exc());raise
