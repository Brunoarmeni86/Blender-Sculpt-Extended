"""Reverse this change only. Verify CMake before applying the reverse patch."""
import argparse,datetime,json,os,shutil,traceback,uuid
from pathlib import Path
from build_helpers import copy_tree,run_native,say,sha256
from compila_e_zip import settings
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True);p.add_argument('--build',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--jobs',type=int,default=4)
a=p.parse_args();kit=Path(__file__).resolve().parent
if os.name!='nt':raise RuntimeError('Richiede Windows')
source,build,output=a.source.resolve(),a.build.resolve(),a.output.resolve()
cache,cmake=settings(build)
if Path(cache.get('CMAKE_HOME_DIRECTORY','')).resolve()!=source:raise RuntimeError('Build/sorgenti diversi')
if output==source or source in output.parents or output==build or build in output.parents:raise RuntimeError('Output non valido')
run=output/('PaintMulti-Ripristino-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6]);run.mkdir(parents=True)
say('LOG E BACKUP RIPRISTINO: '+str(run))
try:
    patch=kit/'polypaint-multi-master.patch';git=['git.exe','-C',str(source),'apply','--ignore-space-change']
    run_native(git+['--check','-R',str(patch)],run/'reverse-check.log','REVERSE_CHECK')
    for entry in json.loads((kit/'SOURCE-HASHES.json').read_text()):
        target=run/'source-before'/entry['path'];target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source/entry['path'],target)
    copy_tree(build/'bin/Release',run/'previous-release','BACKUP_RELEASE')
    run_native(git+['-R',str(patch)],run/'reverse-apply.log','REVERSE_APPLY')
    run_native([str(cmake),'--build',str(build),'--config','Release','--target','INSTALL','--parallel',str(a.jobs)],run/'build.log','BUILD')
    run_native([str(build/'bin/Release/blender.exe'),'--background','--factory-startup','--python-exit-code','1','--python',str(kit/'CONTROLLA-BUILD.py'),'--',str(run/'smoke.json'),'Release'],run/'smoke.log','SMOKE',timeout=180)
    if json.loads((run/'smoke.json').read_text()).get('status')!='PASS':raise RuntimeError('Smoke fallito')
    say('RIPRISTINO COMPLETATO; le altre modifiche sono conservate.')
except BaseException:
    (run/'operation-error.log').write_text(traceback.format_exc());raise
