import json
from pathlib import Path
from analyze_log import records

def delta(a,b):
    if set(a)!=set(b):raise RuntimeError('Livelli geometria differenti')
    result={}
    for level in a:
        if len(a[level])!=len(b[level]):raise RuntimeError('Topologia differente al livello '+level)
        result[level]=max((abs(x-y) for p,q in zip(a[level],b[level]) for x,y in zip(p,q)),default=0.)
    return result

def verify(folder,text,with_reload=False):
    folder=Path(folder);r=json.loads((folder/'ui-test.json').read_text())
    if r['status']!='PASS':raise RuntimeError(r.get('error','Sequenza nativa non completata'))
    items={x['label']:x for x in records(text)}
    wanted=('pre-conform','post-conform','conform-undo','conform-redo','baseline','before-undo','after-undo','after-redo','after-repeat-undo','after-repeat-redo','after-commit','before-save')
    if not set(wanted).issubset(items):raise RuntimeError('Fingerprint mancanti: '+str(set(wanted)-set(items)))
    if any(x['invalid'] or not x['native_available'] for x in items.values()):raise RuntimeError('Fingerprint invalidi')
    if '[LAYER_CCG_UNDO_GUARD] mode=DISCARD_' not in text:raise RuntimeError('La prova non ha esercitato la guardia delle vecchie cache')
    for a,b in (('pre-conform','conform-undo'),('post-conform','conform-redo')):
        for field in ('native_hash','cage_hash','metadata_hash','layer_payloads'):
            if items[a][field]!=items[b][field]:raise RuntimeError('Conform Undo/Redo incoerente: '+a+'/'+b+'/'+field)
    for label in ('before-undo','after-undo','after-redo','after-repeat-undo','after-repeat-redo','after-commit','before-save'):
        for field in ('native_hash','cage_hash'):
            if items[label][field]!=items['baseline'][field]:raise RuntimeError('Base cambiato dopo Conform: '+label+'/'+field)
    if items['baseline']['surface_hash']==items['before-undo']['surface_hash']:raise RuntimeError('La pennellata non ha modificato la superficie')
    if items['baseline']['surface_hash']!=items['after-undo']['surface_hash']:raise RuntimeError('Undo non ripristina la superficie post-Conform')
    for label in ('after-redo','after-repeat-redo'):
        if items[label]['surface_hash']!=items['before-undo']['surface_hash']:raise RuntimeError('Redo non ripristina la pennellata: '+label)
    if items['after-repeat-undo']['surface_hash']!=items['baseline']['surface_hash']:raise RuntimeError('Secondo Undo incoerente')
    hidden=lambda x:[{k:v for k,v in p.items() if k!='index'} for p in x['layer_payloads'] if not p['visible']]
    if hidden(items['post-conform'])!=hidden(items['before-save']):raise RuntimeError('Layer nascosti alterati dopo Conform')
    geometry={}
    for a,b in (('pre-conform','post-conform'),('pre-conform','conform-undo'),('post-conform','conform-redo')):
        changes=delta(r['geometry'][a],r['geometry'][b]);geometry[a+'/'+b]=changes
        # Conform repositions the cage: L0 and lower interpolants may change.
        # Its preservation contract is the full-resolution stored surface.
        checked=[changes[str(r['total'])]] if a=='pre-conform' and b=='post-conform' else list(changes.values())
        if max(checked,default=0)>1e-5:raise RuntimeError('Geometria non preservata: '+a+'/'+b+': '+str(changes))
    if with_reload:
        reload=json.loads((folder/'reload-test.json').read_text())
        if reload['status']!='PASS' or reload['sculpt']!=r['level'] or reload['total']!=r['total']:raise RuntimeError('Riapertura non valida')
        changes=delta(r['geometry']['before-save'],reload['points']);geometry['save/reload']=changes
        if max(changes.values(),default=0)>1e-5:raise RuntimeError('Geometria cambiata alla riapertura')
        if reload['layers']!=r['states']['before-save']['layers']:raise RuntimeError('Layer cambiati alla riapertura')
    return {'status':'PASS','level':r['level'],'base_unchanged_after_stroke_undo':True,'conform_history_checked':True,'hidden_layers_unchanged_after_conform':True,'geometry':geometry,'reload_checked':with_reload,'full_blender_windows_test':True,'large_mesh_tested':False}
