"""One experimental change: independent, stroke-entry Polypaint master promotion."""
import argparse,datetime,hashlib,json,os,re,shutil,subprocess,sys,traceback,uuid
from pathlib import Path
from verify_multi import verify_multi
from build_helpers import copy_tree,filesystem_path,make_zip,run_native,say,sha256


def settings(build):
    result={}
    for line in (build/'CMakeCache.txt').read_text(errors='replace').splitlines():
        if '=' in line and not line.startswith(('#','//')):
            key,value=line.split('=',1);result[key.split(':',1)[0]]=value
    cmake=Path(result.get('CMAKE_COMMAND',''))
    if not cmake.is_file():raise RuntimeError('CMake registrato nella build non trovato: '+str(cmake))
    return result,cmake


def profile():
    env=dict(os.environ)
    flags={
        'BLENDER_SCULPT_LAYER_CACHE_MB':'512','BLENDER_SCULPT_COMPACT_COMMIT_STAGING':'1',
        'BLENDER_SCULPT_FIRST_STROKE_ANCHOR':'1','BLENDER_SCULPT_COMMIT_KEEP_CCG':'1',
        'BLENDER_SCULPT_SKIP_EMPTY_BM_FLUSH':'1','BLENDER_SCULPT_UNDO_BOUNDARY_METADATA':'1',
        'BLENDER_SCULPT_LAYER_STACK_CHECK_V3':'1','BLENDER_SCULPT_LAYER_READONLY_WEIGHT':'1',
        'BLENDER_SCULPT_COMMIT_WORK_PARALLEL':'1','BLENDER_SCULPT_LOCAL_COMMIT':'1',
        'BLENDER_LAYER_RESP_TRACE':'1','BLENDER_SCULPT_COMMIT_PUBLISH_SMALL_GRIDS':'1',
        'BLENDER_SCULPT_PARALLEL_PUBLISH':'1','BLENDER_SCULPT_POLYPAINT_CPU':'1',
        'BLENDER_SCULPT_POLYPAINT_MESH_COLOR':'1','BLENDER_SCULPT_SUBDIVIDE_PRESERVE_RETAINED':'1',
        'BLENDER_SCULPT_SUBDIVIDE_COMPACT':'1','BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE':'1',
        'BLENDER_SCULPT_WEIGHT_SUPPORT_COMPACT':'0','BLENDER_SCULPT_LAYER_SPARSE_READ':'0',
        'BLENDER_SCULPT_POLYPAINT_PREPARED_GRID':'0',
        'BLENDER_SCULPT_POLYPAINT_MULTI_ATTRIBUTE':'1',
        'BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD':'1','BLENDER_SCULPT_LAYER_BAKE_BASE':'1',
        'BLENDER_SCULPT_LAYER_SELECT_ANCHOR':'0','BLENDER_SCULPT_LAYER_COMMIT_ANCHOR':'0',
    }
    env.update(flags);return env


def distance(a,b):
    if len(a)!=len(b):raise RuntimeError('Dimensioni snapshot diverse')
    return max((abs(x-y) for ra,rb in zip(a,b) for x,y in zip(ra,rb)),default=0.)


def verify_snapshots(report,lazy):
    if report.get('status')!='PASS':raise RuntimeError('Lettura snapshot non completata')
    items={x['label']:x for x in report['snapshots']};results=[]
    pairs=[('before','undo-1'),('after-1','redo-1')]
    paints=[('before','after-1')]
    if lazy:
        pairs += [('redo-1','after-subdivide'),('lower-before','lower-undo'),
                  ('lower-after','lower-redo'),('higher-before','higher-undo'),
                  ('higher-after','higher-redo'),('higher-before','promotion-undo'),
                  ('higher-before','promotion-redo'),('higher-after','boundary-redo')]
        paints += [('lower-before','lower-after'),('higher-before','higher-after')]
    for a,b in pairs:
        delta=distance(items[a]['colors'],items[b]['colors'])
        if delta>1e-5:raise RuntimeError(f'Colore non preservato {a}/{b}: {delta}')
        if items[a]['layers']!=items[b]['layers']:raise RuntimeError('Metadati layer modificati: '+a+'/'+b)
        results.append({'a':a,'b':b,'color_delta':delta})
    for a,b in paints:
        delta=distance(items[a]['colors'],items[b]['colors'])
        if delta<1e-6:raise RuntimeError('Paint non ha cambiato il colore: '+b)
        if distance(items[a]['points'],items[b]['points'])>1e-6:
            raise RuntimeError('Paint ha modificato la geometria: '+b)
    return {'status':'PASS','pairs':results,'paint_changed_color_only':True}


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--source',type=Path,required=True);p.add_argument('--build',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--smoke-timeout',type=int,default=180);p.add_argument('--resume-run',type=Path)
    a=p.parse_args();kit=Path(__file__).resolve().parent
    if os.name!='nt':raise RuntimeError('Richiede Windows; qui nessuna patch applicata')
    source,build,output=a.source.resolve(),a.build.resolve(),a.output.resolve()
    cache,cmake=settings(build)
    if Path(cache.get('CMAKE_HOME_DIRECTORY','')).resolve()!=source:
        raise RuntimeError('Build/sorgenti CMake non corrispondono')
    if output==source or source in output.parents or output==build or build in output.parents:
        raise RuntimeError('Output deve essere esterno a sorgenti e build')
    patch=kit/'polypaint-multi-master.patch';manifest=json.loads((kit/'SOURCE-HASHES.json').read_text())
    run=a.resume_run.resolve() if a.resume_run else output/('Polypaint-Multi-Attribute-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6])
    run.mkdir(parents=True,exist_ok=True);say('LOG E BACKUP: '+str(run))
    try:
        git=['git.exe','-C',str(source),'apply','--ignore-space-change']
        if a.resume_run:
            info=json.loads((run/'source-after.json').read_text())
            for entry in info:
                path=source/entry['path']
                if sha256(path)!=entry['sha256']:raise RuntimeError('Sorgenti cambiati dopo la build: '+str(path))
            binary=build/'bin/Release/blender.exe'
            if sha256(binary)!=json.loads((run/'compiled-binary.json').read_text())['sha256']:
                raise RuntimeError('Binario cambiato dopo la compilazione; ripresa rifiutata')
            run_native(git+['--check','-R',str(patch)],run/'resume-patch-check.log','RESUME_CHECK')
        else:
            for entry in manifest:
                contents=(source/entry['path']).read_bytes().replace(b'\r\n',b'\n').rstrip(b'\n')+b'\n'
                if hashlib.sha256(contents).hexdigest()!=entry['before_sha256']:
                    raise RuntimeError('Base sorgente diversa dalla base verificata Polypaint Lazy V02: '+entry['path'])
            run_native(git+['--check',str(patch)],run/'patch-check.log','PATCH_CHECK')
            before=[]
            for entry in manifest:
                path=source/entry['path'];target=run/'source-before'/entry['path'];target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(path,target);before.append({'path':entry['path'],'sha256':sha256(path)})
            (run/'source-before.json').write_text(json.dumps(before,indent=2))
            copy_tree(build/'bin/Release',run/'previous-release','BACKUP_RELEASE')
            run_native(git+[str(patch)],run/'patch-apply.log','PATCH_APPLY')
            run_native([str(cmake),'--build',str(build),'--config','Release','--target','INSTALL','--parallel',str(a.jobs)],run/'build.log','BUILD')
            after=[{'path':x['path'],'sha256':sha256(source/x['path'])} for x in manifest]
            (run/'source-after.json').write_text(json.dumps(after,indent=2))
            (run/'compiled-binary.json').write_text(json.dumps({'sha256':sha256(build/'bin/Release/blender.exe')}))
        portable=run/'Blender-Polypaint-Multi-Attribute'
        copy_tree(build/'bin/Release',portable,'PORTABLE',portable=True)
        env=profile()
        run_native([str(portable/'blender.exe'),'--background','--factory-startup','--python-exit-code','1','--python',str(kit/'CONTROLLA-BUILD.py'),'--',str(run/'smoke-check.json'),'Release'],run/'smoke-check.log','SMOKE',timeout=a.smoke_timeout,env=env)
        smoke=json.loads((run/'smoke-check.json').read_text())
        if smoke.get('status')!='PASS':raise RuntimeError('Smoke check non superato')
        (portable/smoke['version_major_minor']/'config').mkdir(exist_ok=True)
        say('TEST AUTOMATICI: quattro finestre Blender, una alla volta. NON dipingere, cliccare o usare Undo nelle finestre.')
        reports={}
        for variant,flag in [('lazy-layers','1'),('lazy-no-layers','1')]:
            folder=run/('ui-'+variant);folder.mkdir(exist_ok=True)
            current=dict(env);current['BLENDER_SCULPT_POLYPAINT_LAZY_MASTER']=flag
            config=folder/'isolated-config';config.mkdir(exist_ok=True);current['BLENDER_USER_CONFIG']=str(config)
            # Resume must exercise fresh processes, not stale reports from an earlier attempt.
            (folder/'ui-report.json').unlink(missing_ok=True)
            run_native([str(portable/'blender.exe'),'--factory-startup','--disable-autoexec','--python-exit-code','1','--python',str(kit/'VERIFICA-PAINT-UI.py'),'--',str(folder),variant],run/('ui-'+variant+'.log'),'UI_'+variant.upper(),timeout=max(a.smoke_timeout,240),env=current)
            ui=json.loads((folder/'ui-report.json').read_text())
            if ui.get('status')!='PASS':raise RuntimeError('Test UI '+variant+': '+ui.get('error','incompleto'))
            log=(run/('ui-'+variant+'.log')).read_text(errors='replace')
            if flag=='1':
                if not re.search(r'\[POLYPAINT_LAZY_MASTER\] from=0 to=2 geometry=3 undo_boundary=1',log):
                    raise RuntimeError('Primo master non creato al livello Paint 2: '+variant)
                if not re.search(r'\[POLYPAINT_LAZY_MASTER\] from=2 to=3 geometry=4 undo_boundary=1',log):
                    raise RuntimeError('Promozione non rimandata al Paint superiore: '+variant)
                section=log.split('[LAZY_TEST] geometric-subdivide START',1)[1].split('[LAZY_TEST] geometric-subdivide END',1)[0]
                if '[POLYPAINT_LAZY_MASTER]' in section:raise RuntimeError('Colore promosso durante Subdivide')
                if not re.search(r'\[POLYPAINT_CPU\] paint_level=2 master_level=2',log):
                    raise RuntimeError('Writer non ha usato il master indipendente')
            run_native([str(portable/'blender.exe'),'--background','--factory-startup','--disable-autoexec','--python-exit-code','1','--python',str(kit/'LEGGI-SNAPSHOT-UI.py'),'--',str(folder)],run/('snapshots-'+variant+'.log'),'SNAPSHOTS_'+variant.upper(),timeout=max(a.smoke_timeout,300),env=current)
            reports[variant]=json.loads((folder/'snapshot-report.json').read_text())
        validation={variant:verify_snapshots(report,variant!='legacy') for variant,report in reports.items()}
        for variant in ('multi-layers','multi-no-layers'):
            folder=run/('ui-'+variant);folder.mkdir(exist_ok=True)
            current=dict(env);current['BLENDER_SCULPT_POLYPAINT_LAZY_MASTER']='1'
            current['BLENDER_USER_CONFIG']=str(folder/'isolated-config')
            (folder/'isolated-config').mkdir(exist_ok=True)
            (folder/'ui-report.json').unlink(missing_ok=True)
            fixture_variant='lazy-layers' if variant=='multi-layers' else 'lazy-no-layers'
            run_native([str(portable/'blender.exe'),'--factory-startup','--disable-autoexec',
                '--python-exit-code','1','--python',str(kit/'VERIFICA-MULTI-UI.py'),'--',str(folder),fixture_variant],
                run/('ui-'+variant+'.log'),'UI_'+variant.upper(),timeout=max(a.smoke_timeout,480),env=current)
            ui=json.loads((folder/'ui-report.json').read_text())
            if ui.get('status')!='PASS':raise RuntimeError('Test UI '+variant+': '+ui.get('error','incompleto'))
            run_native([str(portable/'blender.exe'),'--background','--factory-startup','--disable-autoexec',
                '--python-exit-code','1','--python',str(kit/'LEGGI-MULTI-SNAPSHOT.py'),'--',str(folder)],
                run/('snapshots-'+variant+'.log'),'SNAPSHOTS_'+variant.upper(),timeout=max(a.smoke_timeout,480),env=current)
            validation[variant]=verify_multi(json.loads((folder/'snapshot-report.json').read_text()))
        (run/'native-validation.json').write_text(json.dumps(validation,indent=2))
        for name in ('experiment-tools','addon'):
            copy_tree(kit/name,portable/name,name.upper())
        for name in ('LOAD-SIDEBAR.py','AVVIA-ESPERIMENTO.ps1'):
            shutil.copy2(kit/name,portable/name)
        # The portable launcher uses its own binary; its internal profile is unchanged.
        launch=portable/'AVVIA-ESPERIMENTO.ps1'
        launch.write_text(launch.read_text().replace("'C:\\BlenderDev\\build_windows_x64_shareable\\bin\\Release\\blender.exe'", "(Join-Path $PSScriptRoot 'blender.exe')"))
        info={'requested_configuration':'Release','experiment':'Polypaint Multi Attribute V01',
              'native_validation':validation,'first_paint_undo_redo':True,
              'promotion_undo_redo':True,'delete_higher_resubdivide_color_preserved':True,
              'first_sculpt_stroke_undo_tested':False,'large_mesh_performance_tested':False,
              'position_undo_implementation_modified':False,'color_promotion_undo_boundaries_added':True,
              'binary_sha256':sha256(portable/'blender.exe'),'source_files':json.loads((run/'source-after.json').read_text())}
        (portable/'BUILD-INFO.json').write_text(json.dumps(info,indent=2))
        archive=run/(run.name+'.zip')
        if archive.exists() or archive.with_suffix('.zip.partial').exists():
            archive=run/(run.name+'-'+uuid.uuid4().hex[:6]+'.zip')
        make_zip(portable,archive)
        say('COMPLETATO\nPORTABLE: '+str(portable)+'\nZIP: '+str(archive)+'\nAVVIO: '+str(launch))
    except BaseException:
        (run/'operation-error.log').write_text(traceback.format_exc(),encoding='utf-8')
        say('OPERAZIONE NON COMPLETATA. Log: '+str(run/'operation-error.log'));raise


if __name__=='__main__':
    main()
