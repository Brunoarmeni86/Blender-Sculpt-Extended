"""Reverse only this experiment; preserve current sources and Release first."""
import argparse,datetime,json,os,shutil,traceback,uuid
from pathlib import Path
from compila_e_zip import copy_tree,run_native,say,sha256
p=argparse.ArgumentParser()
p.add_argument('--source',required=True,type=Path);p.add_argument('--build',required=True,type=Path)
p.add_argument('--output',required=True,type=Path);p.add_argument('--jobs',type=int,default=4)
a=p.parse_args();kit=Path(__file__).resolve().parent
source,build,output=a.source.resolve(),a.build.resolve(),a.output.resolve()
if os.name!='nt':raise RuntimeError('Richiede Windows/VS2022')
for base in (source,build):
    if output==base or base in output.parents:raise RuntimeError('Output deve essere esterno a sorgenti/build')
settings={}
for line in (build/'CMakeCache.txt').read_text(errors='replace').splitlines():
    if '=' in line and not line.startswith(('#','//')):
        key,value=line.split('=',1);settings[key.split(':',1)[0]]=value
if Path(settings.get('CMAKE_HOME_DIRECTORY','')).resolve()!=source:raise RuntimeError('Build/sorgenti non corrispondono')
run=output/('Performance-Ripristino-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6]);run.mkdir(parents=True)
say('LOG E BACKUP RIPRISTINO: '+str(run))
try:
    patch=kit/'sculpt-performance.patch';git=['git.exe','-C',str(source),'apply','--ignore-space-change']
    run_native(git+['--check','-R',str(patch)],run/'reverse-check.log','REVERSE_CHECK')
    before=[]
    for entry in json.loads((kit/'SOURCE-HASHES.json').read_text()):
        path=source/entry['path'];target=run/'source-before'/entry['path'];target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(path,target);before.append({'path':entry['path'],'sha256':sha256(path)})
    (run/'source-before.json').write_text(json.dumps(before,indent=2))
    copy_tree(build/'bin/Release',run/'previous-release','BACKUP_RELEASE')
    run_native(git+['-R',str(patch)],run/'reverse-apply.log','REVERSE_APPLY')
    run_native(['cmake.exe','--build',str(build),'--config','Release','--target','INSTALL','--parallel',str(a.jobs)],run/'build.log','BUILD')
    binary=build/'bin/Release/blender.exe'
    run_native([str(binary),'--background','--factory-startup','--python-exit-code','1','--python',str(kit/'CONTROLLA-BUILD.py'),'--',str(run/'smoke-check.json'),'Release'],run/'smoke-check.log','SMOKE',timeout=180)
    if json.loads((run/'smoke-check.json').read_text()).get('status')!='PASS':raise RuntimeError('Smoke check non superato')
    say('RIPRISTINO COMPLETATO. Correzione precisione e ottimizzazioni precedenti conservate. Portable precedenti conservate.')
except BaseException:
    (run/'operation-error.log').write_text(traceback.format_exc(),encoding='utf-8');raise
