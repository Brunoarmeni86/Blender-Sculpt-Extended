"""Repeat native tests on the already compiled candidate, without INSTALL."""
import argparse,datetime,hashlib,json
from pathlib import Path
from runner import stable_profile
from build_helpers import run_native,say
from verify_results import verify

def main():
    p=argparse.ArgumentParser();p.add_argument('--run',type=Path,required=True);p.add_argument('--timeout',type=int,default=300);a=p.parse_args()
    run=a.run.resolve();kit=Path(__file__).resolve().parent
    info=json.loads((run/'run-info.json').read_text())
    source=Path(info['source'])
    for entry in info['manifest']:
        path=source/entry['path']
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=entry['after_sha256']:
            raise RuntimeError('Sorgente diverso dalla candidata compilata: '+entry['path'])
    binary=run/'Blender-Conform-Undo-Cache/blender.exe'
    if not binary.is_file():raise RuntimeError('Portable della prova precedente mancante: '+str(binary))
    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    results=[];env=stable_profile()
    try:
        for level in (2,5):
            folder=run/('retest-fix02-L'+str(level)+'-'+stamp);folder.mkdir()
            log=folder/'native.log'
            run_native([str(binary),'--factory-startup','--disable-autoexec','--python-exit-code','1','--python',str(kit/'VERIFICA-CONFORM-UNDO.py'),'--',str(folder),str(level)],log,'RETEST_L'+str(level),timeout=a.timeout,env=env)
            verify(folder,log.read_text(encoding='utf-8',errors='replace'))
            run_native([str(binary),'--background','--factory-startup','--disable-autoexec','--python-exit-code','1','--python',str(kit/'VERIFICA-RIAPERTURA.py'),'--',str(folder)],folder/'reload.log','RELOAD_L'+str(level),timeout=a.timeout,env=env)
            result=verify(folder,log.read_text(encoding='utf-8',errors='replace'),with_reload=True)
            results.append(result);(folder/'validation.json').write_text(json.dumps(result,indent=2))
        (run/('validation-testfix02-'+stamp+'.json')).write_text(json.dumps(results,indent=2))
        (kit/'ULTIMA-BUILD.json').write_text(json.dumps({'BlenderExe':str(binary),'RunDir':str(run)},indent=2))
        say('PROVE NATIVE COMPLETATE: test Python corretti, Conform/Undo e riapertura PASS.')
    except Exception:
        say('TEST NON COMPLETATO. Log nuovi in: '+str(folder));raise

if __name__=='__main__':main()
