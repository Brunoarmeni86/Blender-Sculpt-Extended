"""Windows build of an isolated Memfile/CCG guard; preserves existing sculpt undo."""
import argparse,datetime,difflib,hashlib,json,os,shutil,traceback,uuid
from pathlib import Path
from build_helpers import copy_tree,filesystem_path,run_native,say,sha256
from patch_sources import prepare
from verify_results import verify

def digest(data):return hashlib.sha256(data).hexdigest()
def settings(build):
    values={}
    for line in (build/'CMakeCache.txt').read_text(errors='replace').splitlines():
        if '=' in line and not line.startswith(('#','//')):
            k,v=line.split('=',1);values[k.split(':',1)[0]]=v
    cmake=Path(values.get('CMAKE_COMMAND',''))
    if not cmake.is_file():raise RuntimeError('CMake della build non trovato: '+str(cmake))
    return values,cmake

def stable_profile():
    env=dict(os.environ)
    env.update(json.loads((Path(__file__).parent/'PROFILE.json').read_text()))
    return env

def write(path,data):
    path=filesystem_path(path);path.parent.mkdir(parents=True,exist_ok=True)
    temp=path.with_name(path.name+'.conform-cache-tmp')
    temp.write_bytes(data);os.replace(temp,path)

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--source',type=Path,required=True);p.add_argument('--build',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--smoke-timeout',type=int,default=300);p.add_argument('--resume-run',type=Path)
    p.add_argument('--restore',action='store_true');a=p.parse_args()
    if os.name!='nt':raise RuntimeError('Runner riservato a Windows; nessuna modifica eseguita.')
    source,build,output=a.source.resolve(),a.build.resolve(),a.output.resolve()
    cache,cmake=settings(build)
    if Path(cache.get('CMAKE_HOME_DIRECTORY','')).resolve()!=source:raise RuntimeError('Build e sorgenti non corrispondono.')
    if not 1<=a.jobs<=256:raise RuntimeError('Numero jobs invalido.')
    kit=Path(__file__).resolve().parent
    run=a.resume_run.resolve() if a.resume_run else output/('Conform-Undo-Cache-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6])
    for path in (output,run):
        if path==source or source in path.parents or path==build or build in path.parents:
            raise RuntimeError('Output/RunDir deve essere esterno a sorgenti e build.')
    if a.restore and not a.resume_run:raise RuntimeError('Ripristino richiede RunDir.')
    run.mkdir(parents=True,exist_ok=True);say('LOG E BACKUP: '+str(run))
    (kit/'ULTIMO-RUN.txt').write_text(str(run)+'\n')
    try:
        if a.resume_run:
            info=json.loads((run/'run-info.json').read_text())
            if Path(info['source']).resolve()!=source or Path(info['build']).resolve()!=build:raise RuntimeError('RunDir appartiene a un altro sorgente/build.')
            for e in info['manifest']:
                path=source/e['path'];present=path.is_file()
                expected=e['after_sha256']
                if a.restore and ((e['before_sha256'] is None and not present) or (present and digest(path.read_bytes())==e['before_sha256'])):continue
                if not present or digest(path.read_bytes())!=expected:raise RuntimeError('Sorgente cambiato dopo la patch: '+e['path'])
            if a.restore:
                copy_tree(build/'bin/Release',run/('before-restore-'+uuid.uuid4().hex[:6]),'BACKUP_CURRENT')
                for e in info['manifest']:
                    target=source/e['path']
                    if e['before_sha256'] is None:
                        if target.exists():target.unlink()
                    else:write(target,(run/'source-before'/e['path']).read_bytes())
        else:
            outputs=prepare(source,kit) # All anchor checks before any mutation.
            manifest=[];patch=[]
            for rel,item in outputs.items():
                if item['before'] is not None:
                    write(run/'source-before'/rel,item['before'])
                manifest.append({'path':rel,'before_sha256':digest(item['before']) if item['before'] is not None else None,'after_sha256':digest(item['after'])})
                patch.extend(difflib.unified_diff((item['before'] or b'').decode().replace('\r\n','\n').splitlines(True),item['after'].decode().replace('\r\n','\n').splitlines(True),fromfile='a/'+rel if item['before'] else '/dev/null',tofile='b/'+rel))
            copy_tree(build/'bin/Release',run/'previous-release','BACKUP_RELEASE')
            (run/'PATCH.diff').write_text(''.join(patch))
            info={'source':str(source),'build':str(build),'manifest':manifest,'changes':'CCG writes during global Undo + post-Conform layer cache invalidation','sculpt_undo_cc_changed':False}
            (run/'run-info.json').write_text(json.dumps(info,indent=2))
            # A failure during publication restores already written sources.
            applied=[]
            try:
                for rel,item in outputs.items():write(source/rel,item['after']);applied.append(rel)
            except Exception:
                for rel in reversed(applied):
                    before=outputs[rel]['before']
                    if before is None:(source/rel).unlink(missing_ok=True)
                    else:write(source/rel,before)
                raise
        run_native([str(cmake),'--build',str(build),'--config','Release','--target','INSTALL','--parallel',str(a.jobs)],run/('restore-build.log' if a.restore else 'build.log'),'BUILD')
        if a.restore:say('RIPRISTINO COMPLETATO: sola patch Conform/Undo Cache rimossa.');return
        portable=run/'Blender-Conform-Undo-Cache'
        copy_tree(build/'bin/Release',portable,'PORTABLE',portable=True)
        (portable/'5.2/config').mkdir(parents=True,exist_ok=True)
        binary=portable/'blender.exe';env=stable_profile();results=[]
        for level in (2,5):
            folder=run/('native-L'+str(level));folder.mkdir(exist_ok=True)
            log=run/('native-L'+str(level)+'.log')
            run_native([str(binary),'--factory-startup','--disable-autoexec','--python-exit-code','1','--python',str(kit/'VERIFICA-CONFORM-UNDO.py'),'--',str(folder),str(level)],log,'NATIVE_L'+str(level),timeout=max(a.smoke_timeout,300),env=env)
            results.append(verify(folder,log.read_text(encoding='utf-8',errors='replace')))
            run_native([str(binary),'--background','--factory-startup','--disable-autoexec','--python-exit-code','1','--python',str(kit/'VERIFICA-RIAPERTURA.py'),'--',str(folder)],run/('reload-L'+str(level)+'.log'),'RELOAD_L'+str(level),timeout=max(a.smoke_timeout,300),env=env)
            results[-1]=verify(folder,log.read_text(encoding='utf-8',errors='replace'),with_reload=True)
        (run/'validation.json').write_text(json.dumps(results,indent=2))
        (kit/'ULTIMA-BUILD.json').write_text(json.dumps({'BlenderExe':str(binary),'RunDir':str(run)},indent=2))
        say('PROVE NATIVE COMPLETATE: Conform, Undo/Redo e riapertura PASS.');say('AVVIO: '+str(kit/'AVVIA-CORREZIONE.ps1'))
    except BaseException:
        (run/'operation-error.log').write_text(traceback.format_exc(),encoding='utf-8');say('ERRORE: '+str(run/'operation-error.log'));raise

if __name__=='__main__':main()
