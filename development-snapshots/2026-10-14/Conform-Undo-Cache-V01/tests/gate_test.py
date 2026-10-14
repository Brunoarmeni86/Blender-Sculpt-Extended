"""Reject damaged base/layer history; fixtures model only fingerprint outputs."""
import copy,json,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from verify_results import verify

labels=['pre-conform','post-conform','conform-undo','conform-redo','baseline','before-undo','after-undo','after-redo','after-repeat-undo','after-repeat-redo','after-commit','before-save']
rows={}
hidden={'index':0,'name':'hidden','visible':0,'strength':1,'grids_hash':'hidden'}
visible={'index':1,'name':'main','visible':1,'strength':.6,'grids_hash':'main'}
for label in labels:
    pre=label in ('pre-conform','conform-undo')
    rows[label]={'label':label,'invalid':0,'native_available':1,'native_hash':'old' if pre else 'conformed',
        'cage_hash':'old-cage' if pre else 'new-cage','metadata_hash':'meta',
        'surface_hash':'stroke' if label in ('before-undo','after-redo','after-repeat-redo') else 'neutral',
        'layer_payloads':[copy.deepcopy(hidden),copy.deepcopy(visible)]}
points={str(level):[[0,0,0]] for level in range(6)}
report={'status':'PASS','level':2,'total':5,'geometry':{label:points for label in ('pre-conform','post-conform','conform-undo','conform-redo','before-save')},'states':{'before-save':{'layers':[]}}}
def log():
    result=['[LAYER_CCG_UNDO_GUARD] mode=DISCARD_PENDING direction=-1']
    for label in labels:
        result.append('[MULTIRES_AUDIT_BEGIN] '+json.dumps(label))
        result.extend('[MULTIRES_LAYER_AUDIT] '+json.dumps(x) for x in rows[label]['layer_payloads'])
        result.append('[MULTIRES_BASE_AUDIT] '+json.dumps({k:v for k,v in rows[label].items() if k!='layer_payloads'}))
    return '\n'.join(result)
with tempfile.TemporaryDirectory() as tmp:
    folder=Path(tmp);(folder/'ui-test.json').write_text(json.dumps(report))
    assert verify(folder,log())['status']=='PASS'
    rows['after-undo']['native_hash']='damaged-base'
    try:verify(folder,log())
    except RuntimeError as e:assert 'Base cambiato' in str(e)
    else:raise AssertionError('Missed base damage')
    rows['after-undo']['native_hash']='conformed'
    rows['conform-redo']['layer_payloads'][1]['grids_hash']='wrong-frame'
    try:verify(folder,log())
    except RuntimeError as e:assert 'Conform Undo/Redo incoerente' in str(e)
    else:raise AssertionError('Missed layer frame damage')
    rows['conform-redo']['layer_payloads'][1]['grids_hash']='main'
    try:verify(folder,log().replace('[LAYER_CCG_UNDO_GUARD] mode=DISCARD_PENDING','unexercised'))
    except RuntimeError as e:assert 'esercitato' in str(e)
    else:raise AssertionError('Accepted an unexercised guard')
print('PASS: baseline fixture, base-damage rejection, layer-frame rejection, unexercised-guard rejection')
