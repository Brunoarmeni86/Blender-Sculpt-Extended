"""Prepared Paint V02: incremental brush-only patch and native prepared/reference comparison."""
import argparse,datetime,hashlib,json,os,re,shutil,subprocess,sys,traceback,uuid
from pathlib import Path
from verify_multi import verify_multi
from prepare_patch import prepare_patch, sha
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
        'BLENDER_SCULPT_POLYPAINT_PREPARED_V2':'1',
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
    run=a.resume_run.resolve() if a.resume_run else output/('Polypaint-Prepared-V02-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6])
    run.mkdir(parents=True,exist_ok=True);say('LOG E BACKUP: '+str(run))
    try:
        git=['git.exe','-C',str(source),'apply','--ignore-space-change']
        if a.resume_run:
            patch=run/'applied.patch'
            manifest=json.loads((run/'source-manifest.json').read_text())
            info=json.loads((run/'source-after.json').read_text())
            for entry in info:
                if sha256(source/entry['path'])!=entry['sha256']:
                    raise RuntimeError('Sorgenti cambiati dopo la patch: '+entry['path'])
            if not (run/'compiled-binary.json').exists():
                raise RuntimeError('La compilazione precedente non e completata; conserva build.log. COMPLETA-BUILD ripete i test solo dopo una build completata')
            binary=build/'bin/Release/blender.exe'
            if sha256(binary)!=json.loads((run/'compiled-binary.json').read_text())['sha256']:
                raise RuntimeError('Binario cambiato dopo la compilazione; ripresa rifiutata')
            run_native(git+['--check','-R',str(patch)],run/'resume-patch-check.log','RESUME_CHECK')
        else:
            patch,manifest=prepare_patch(source,run,kit)
            run_native(git+['--check',str(patch)],run/'patch-check.log','PATCH_CHECK')
            before=[]
            for entry in manifest:
                path=source/entry['path']
                exists=path.exists()
                if exists:
                    target=run/'source-before'/entry['path'];target.parent.mkdir(parents=True,exist_ok=True)
                    shutil.copy2(path,target)
                before.append({'path':entry['path'],'sha256':sha256(path) if exists else None})
            (run/'source-before.json').write_text(json.dumps(before,indent=2))
            copy_tree(build/'bin/Release',run/'previous-release','BACKUP_RELEASE')
            run_native(git+[str(patch)],run/'patch-apply.log','PATCH_APPLY')
            after=[{'path':x['path'],'sha256':sha256(source/x['path'])} for x in manifest]
            (run/'source-after.json').write_text(json.dumps(after,indent=2))
            (kit/'last-prepared-run.json').write_text(json.dumps({'run_dir':str(run)}))
            run_native([str(cmake),'--build',str(build),'--config','Release','--target','INSTALL','--parallel',str(a.jobs)],run/'build.log','BUILD')
            (run/'compiled-binary.json').write_text(json.dumps({'sha256':sha256(build/'bin/Release/blender.exe')}))
        portable=run/'Blender-Polypaint-Prepared-V02'
        copy_tree(build/'bin/Release',portable,'PORTABLE',portable=True)
        env=profile()
        run_native([str(portable/'blender.exe'),'--background','--factory-startup','--python-exit-code','1','--python',str(kit/'CONTROLLA-BUILD.py'),'--',str(run/'smoke-check.json'),'Release'],run/'smoke-check.log','SMOKE',timeout=a.smoke_timeout,env=env)
        smoke=json.loads((run/'smoke-check.json').read_text())
        if smoke.get('status')!='PASS':raise RuntimeError('Smoke check non superato')
        (portable/smoke['version_major_minor']/'config').mkdir(exist_ok=True)
        say('TEST AUTOMATICI: sei finestre Blender, una alla volta. NON dipingere, cliccare o usare Undo nelle finestre.')
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
        multi_reports={}
        variants=[('multi-layers','lazy-layers','1'),('multi-no-layers','lazy-no-layers','1'),
                  ('reference-layers','lazy-layers','0'),('reference-no-layers','lazy-no-layers','0')]
        for variant,fixture_variant,prepared_flag in variants:
            folder=run/('ui-'+variant);folder.mkdir(exist_ok=True)
            current=dict(env);current['BLENDER_SCULPT_POLYPAINT_LAZY_MASTER']='1'
            current['BLENDER_USER_CONFIG']=str(folder/'isolated-config')
            (folder/'isolated-config').mkdir(exist_ok=True)
            (folder/'ui-report.json').unlink(missing_ok=True)
            current['BLENDER_SCULPT_POLYPAINT_PREPARED_V2']=prepared_flag
            run_native([str(portable/'blender.exe'),'--factory-startup','--disable-autoexec',
                '--python-exit-code','1','--python',str(kit/'VERIFICA-MULTI-UI.py'),'--',str(folder),fixture_variant],
                run/('ui-'+variant+'.log'),'UI_'+variant.upper(),timeout=max(a.smoke_timeout,480),env=current)
            ui=json.loads((folder/'ui-report.json').read_text())
            if ui.get('status')!='PASS':raise RuntimeError('Test UI '+variant+': '+ui.get('error','incompleto'))
            run_native([str(portable/'blender.exe'),'--background','--factory-startup','--disable-autoexec',
                '--python-exit-code','1','--python',str(kit/'LEGGI-MULTI-SNAPSHOT.py'),'--',str(folder)],
                run/('snapshots-'+variant+'.log'),'SNAPSHOTS_'+variant.upper(),timeout=max(a.smoke_timeout,480),env=current)
            multi_reports[variant]=json.loads((folder/'snapshot-report.json').read_text())
            validation[variant]=verify_multi(multi_reports[variant])
            log=(run/('ui-'+variant+'.log')).read_text(errors='replace')
            if prepared_flag=='1' and not any(int(n)>0 for n in re.findall(
                r'\[POLYPAINT_PREPARED_V2\] enabled=1 reused_samples=(\d+)',log)):
                raise RuntimeError('Il percorso preparato non e stato esercitato: '+variant)
        ab=[]
        for prepared,reference in [('multi-layers','reference-layers'),('multi-no-layers','reference-no-layers')]:
            left={x['label']:x for x in multi_reports[prepared]['snapshots']}
            right={x['label']:x for x in multi_reports[reference]['snapshots']}
            if set(left)!=set(right):raise RuntimeError('Snapshot A/B diversi')
            for label,x in left.items():
                y=right[label]
                if x['levels']!=y['levels'] or x['active']!=y['active'] or x['layers']!=y['layers']:
                    raise RuntimeError('Metadati A/B diversi: '+label)
                if set(x['colors'])!=set(y['colors']):raise RuntimeError('Attributi A/B diversi')
                maximum=max(distance(x['colors'][name],y['colors'][name]) for name in x['colors'])
                if maximum>1e-6:raise RuntimeError('Colore prepared/reference diverso: '+label+' '+str(maximum))
                if distance(x['points'],y['points'])>1e-6:raise RuntimeError('Geometria A/B diversa')
                ab.append({'variant':prepared,'label':label,'max_color_delta':maximum})
        validation['prepared_reference_ab']={'status':'PASS','comparisons':ab}
        (run/'paint-profiles.json').write_text(json.dumps({name:{
            'ui_paint_seconds':json.loads((run/('ui-'+name)/'ui-report.json').read_text())['paint_seconds'],
            'kernel_events':re.findall(r'\[POLYPAINT_PREPARED_V2\][^\r\n]*',
                (run/('ui-'+name+'.log')).read_text(errors='replace'))} for name,_,_ in variants},indent=2))
        (run/'native-validation.json').write_text(json.dumps(validation,indent=2))
        for name in ('experiment-tools','addon'):
            copy_tree(kit/name,portable/name,name.upper())
        for name in ('LOAD-SIDEBAR.py','AVVIA-ESPERIMENTO.ps1'):
            shutil.copy2(kit/name,portable/name)
        # The portable launcher uses its own binary; its internal profile is unchanged.
        launch=portable/'AVVIA-ESPERIMENTO.ps1'
        launch.write_text(launch.read_text().replace("'C:\\BlenderDev\\build_windows_x64_shareable\\bin\\Release\\blender.exe'", "(Join-Path $PSScriptRoot 'blender.exe')"))
        info={'requested_configuration':'Release','experiment':'Polypaint Prepared V02',
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
