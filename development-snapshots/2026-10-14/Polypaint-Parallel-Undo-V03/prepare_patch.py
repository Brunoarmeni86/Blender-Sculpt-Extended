"""Read-only preflight; build exact reversible patch over Prepared V02."""
import difflib,hashlib,json
PAINT='source/blender/editors/sculpt_paint/mesh/sculpt_paint_color.cc'
UNDO='source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc'
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
            raise RuntimeError('Base verificata diversa: '+entry['path'])
    if sha((source/HEADER).read_bytes())!=sha((kit/'BKE_multires_polypaint_prepared.hh').read_bytes()):
        raise RuntimeError('Richiede Prepared V02 verificata: header diverso')
    text=(source/PAINT).read_bytes().decode('utf-8').replace('\r\n','\n')
    a,b,old=function(text)
    if sha(old.encode())!=sha((kit/'reference-prepared-v02.cc.txt').read_bytes()):
        (run/'paint-function-observed.cc.txt').write_text(old)
        raise RuntimeError('Funzione Paint diversa dalla Prepared V02; nessun sorgente modificato')
    after=text[:a]+(kit/'paint-function-v03.cc.txt').read_text()+text[b:]
    anchor='#include "BKE_multires_polypaint_prepared.hh"'
    if after.count(anchor)!=1:raise RuntimeError('Include Prepared V02 inatteso')
    if '#include <unordered_set>' not in after:after=after.replace(anchor,anchor+'\n#include <unordered_set>\n#include <vector>')
    before_undo=(source/UNDO).read_bytes().decode('utf-8').replace('\r\n','\n')
    if sha(before_undo.encode())!=sha((kit/'reference-undo.cc.txt').read_bytes()):
        raise RuntimeError('Undo diverso dalla base verificata')
    new_undo=(kit/'undo-v03.cc.txt').read_text()
    patch=[];manifest=[]
    for rel,before,new in ((PAINT,text,after),(UNDO,before_undo,new_undo)):
        patch.append('diff --git a/'+rel+' b/'+rel+'\n')
        patch.extend(difflib.unified_diff(before.splitlines(True),new.splitlines(True),fromfile='a/'+rel,tofile='b/'+rel))
        manifest.append({'path':rel,'before_sha256':sha(before.encode()),'after_sha256':sha(new.encode())})
    path=run/'applied.patch';path.write_text(''.join(patch),encoding='utf-8',newline='\n')
    (run/'source-manifest.json').write_text(json.dumps(manifest,indent=2))
    return path,manifest
