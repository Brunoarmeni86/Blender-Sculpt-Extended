"""Optional installer check. Uses temporary copies only; --source is read-only."""
from pathlib import Path
import argparse,sys,subprocess,json,tempfile
kit=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(kit));from prepare_patch import prepare_patch,sha,function,PAINT,HEADER
parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True)
args=parser.parse_args();base=json.loads((kit/'BASE-HASHES.json').read_text());cases=0
for ref in ('reference-legacy-v02b.cc.txt','reference-prepared-v01.cc.txt'):
 for crlf in (False,True):
  for blank in ('','\n\n'):
   with tempfile.TemporaryDirectory(prefix='prepared-v02-') as temp:
    source=Path(temp)/'source';run=Path(temp)/'run';run.mkdir();source.mkdir()
    for entry in base:
     target=source/entry['path'];target.parent.mkdir(parents=True,exist_ok=True)
     target.write_bytes((args.source/entry['path']).read_bytes())
    text='// Preserved user comment\n#include "BKE_multires_sculpt_layers.hh"\n'+(kit/ref).read_text()+'\nvoid do_paint_brush() {}\n'+blank
    raw=text.replace('\n','\r\n').encode() if crlf else text.encode()
    target=source/PAINT;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
    subprocess.run(['git','init','-q',str(source)],check=True)
    patch,manifest=prepare_patch(source,run,kit)
    subprocess.run(['git','-C',str(source),'apply','--ignore-space-change',str(patch)],check=True)
    assert target.read_text().startswith('// Preserved user comment\n')
    assert function(target.read_text())[2]==(kit/'paint-function-v02.cc.txt').read_text()
    for entry in manifest:assert sha((source/entry['path']).read_bytes())==entry['after_sha256']
    subprocess.run(['git','-C',str(source),'apply','--ignore-space-change','-R',str(patch)],check=True)
    assert sha(target.read_bytes())==sha(raw) and not (source/HEADER).exists()
    target.write_text(target.read_text().replace('std::abs(ss.cache->bstrength)','std::abs(ss.cache->bstrength) * 2.0f',1))
    saved=target.read_bytes()
    try:prepare_patch(source,run,kit)
    except RuntimeError:pass
    else:raise AssertionError('Unknown brush math accepted')
    assert target.read_bytes()==saved and not (source/HEADER).exists();cases+=1
print('PASS',cases,'temporary installer cases; actual source tree unchanged')
