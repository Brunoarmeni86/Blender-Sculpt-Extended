"""Validate the exact known brush function; preserve every other byte of source logic.
Patch construction and baseline checks are read-only with respect to Blender sources.
"""
import difflib,hashlib,json
from pathlib import Path
PAINT='source/blender/editors/sculpt_paint/mesh/sculpt_paint_color.cc'
HEADER='source/blender/blenkernel/BKE_multires_polypaint_prepared.hh'
def canonical(data):return data.replace(b'\r\n',b'\n').rstrip(b'\n')+b'\n'
def sha(data):return hashlib.sha256(canonical(data)).hexdigest()
def function(text):
    start=text.index('static void do_paint_brush_grids_cpu(')
    end=text.index('\nvoid do_paint_brush(',start)
    return start,end,text[start:end]
def prepare_patch(source,run,kit):
    for entry in json.loads((kit/'BASE-HASHES.json').read_text()):
        if sha((source/entry['path']).read_bytes())!=entry['sha256']:
            raise RuntimeError('Base multi-attributo verificata diversa: '+entry['path'])
    if (source/HEADER).exists():raise RuntimeError('Header Prepared V02 gia presente: usa COMPLETA-BUILD dopo una build completata, oppure RIPRISTINA con il RunDir originale')
    original=(source/PAINT).read_bytes();text=original.decode('utf-8').replace('\r\n','\n')
    a,b,old=function(text)
    references=[(kit/n).read_bytes() for n in ('reference-legacy-v02b.cc.txt','reference-prepared-v01.cc.txt')]
    if sha(old.encode()) not in [sha(r) for r in references]:
        (run/'paint-function-observed.cc.txt').write_text(old)
        raise RuntimeError('Funzione Paint diversa dai due riferimenti conosciuti; nessun sorgente modificato. Allega paint-function-observed.cc.txt da '+str(run))
    replacement=(kit/'paint-function-v02.cc.txt').read_text()
    after=text[:a]+replacement+text[b:]
    anchor='#include "BKE_multires_sculpt_layers.hh"'
    if after.count(anchor)!=1:raise RuntimeError('Include Paint inatteso: nessun sorgente modificato')
    after=after.replace(anchor,anchor+'\n#include "BKE_multires_polypaint_prepared.hh"')
    header=(kit/'BKE_multires_polypaint_prepared.hh').read_text()
    patch=[];manifest=[]
    for rel,before,new in ((PAINT,text,after),(HEADER,'',header)):
        patch.append('diff --git a/'+rel+' b/'+rel+'\n')
        if not before:patch.append('new file mode 100644\n')
        patch.extend(difflib.unified_diff(before.splitlines(True),new.splitlines(True),
            fromfile=('a/'+rel) if before else '/dev/null',tofile='b/'+rel))
        manifest.append({'path':rel,'before_sha256':sha(before.encode()) if before else None,'after_sha256':sha(new.encode())})
    path=run/'applied.patch';path.write_text(''.join(patch),encoding='utf-8',newline='\n')
    (run/'source-manifest.json').write_text(json.dumps(manifest,indent=2))
    return path,manifest
