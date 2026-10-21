"""Validate installed approved V05 defaults, No-REC and native geometry."""
import argparse
import json
import os
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--blender',type=Path,required=True)
parser.add_argument('--reference',type=Path)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--startup-only',action='store_true')
args=parser.parse_args()
kit=Path(__file__).resolve().parent
args.output.mkdir(parents=True,exist_ok=True)
environment={k:v for k,v in os.environ.items() if not k.startswith(('BLENDER_','FACESETS_','SUBDIV_','BAKE_'))}
def run(executable,script,log,extra=None,background=False,arguments=None):
    env=environment.copy();env.update(extra or {})
    command=[str(executable.resolve()),'--factory-startup','--python-exit-code','1']
    command+=['--background'] if background else ['--no-window-focus','--window-geometry','0','0','1280','800']
    command+=['--python',str(kit/script)]
    if arguments:command+=['--',*arguments]
    startup=None
    if os.name=='nt':
        startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    with log.open('w',encoding='utf-8') as stream:
        subprocess.run(command,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=1200,startupinfo=startup,check=True)
for mode in ('normal','override','optout'):
    extra={'BLENDER_SCULPT_LAYER_CACHE_MB':'384'} if mode=='override' else {'BLENDER_SCULPT_EXTENDED_NATIVE':'0'} if mode=='optout' else {}
    run(args.blender,'test_startup.py',args.output/(mode+'.log'),extra,True,[mode,str(args.output.resolve())])
    print(mode,'PASS',flush=True)
if args.startup_only:raise SystemExit(0)
summary={}
cases=[('bake_HR','BAKE_WORKER.py',{'BAKE_L0':'0'}),('bake_L0','BAKE_WORKER.py',{'BAKE_L0':'1'}),('subdivide_affine','SUBDIV_WORKER.py',{'BLENDER_MULTIRES_DETAIL_TRANSPORT':'affine'}),('subdivide_rotation','SUBDIV_WORKER.py',{'BLENDER_MULTIRES_DETAIL_TRANSPORT':'rotation'})]
for label,worker,settings in cases:
    targets=[('reference',args.reference),('candidate',args.blender)] if args.reference else [('candidate',args.blender)]
    for kind,binary in targets:
        folder=(args.output/label/kind).resolve();folder.mkdir(parents=True,exist_ok=True)
        for phase in ('seed','exercise','reopen'):
            extra=dict(settings,SUBDIV_TEST_ROOT=str(folder),SUBDIV_TEST_PHASE=phase,SUBDIV_SEED=str(folder/'seed.blend'),SUBDIV_REFERENCE=str(folder.parent/'reference') if args.reference and kind=='candidate' else '',BLENDER_SCULPT_SUBDIVIDE_PROFILE='1')
            run(binary,worker,folder/(phase+'.log'),extra)
            data=json.loads((folder/(phase+'.json')).read_text())
            assert data['status'].startswith('PASS'),data.get('error',data)
            summary[label+'/'+kind+'/'+phase]=data
            print(label,kind,phase,'PASS',flush=True)
    if label.startswith('subdivide'):
        text=(args.output/label/'candidate/exercise.log').read_text(errors='replace')
        for marker in ('[SUBDIVIDE_COMBINED_TOPOLOGY_V05]','[SUBDIVIDE_COMBINED_STAGING_V05]','[SUBDIVIDE_REGIONAL_V03] state=ACTIVE','[SUBDIVIDE_MASK_SCRATCH_V04]','base_cache=0'):
            assert marker in text,(label,marker)
(args.output/'RESULT.json').write_text(json.dumps({'status':'PASS','cases':summary},indent=2))
