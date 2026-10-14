"""Native comparison: full coordinates and attributes, no invented timings."""
import math

def error(a,b):
    if len(a)!=len(b):raise RuntimeError('Topology/attribute length differs')
    largest=0.
    for p,q in zip(a,b):
        if len(p)!=len(q):raise RuntimeError('Component count differs')
        for x,y in zip(p,q):
            if not math.isfinite(x) or not math.isfinite(y):raise RuntimeError('Non-finite coordinate/color')
            largest=max(largest,abs(x-y))
    return largest

def compare(legacy,compact):
    if legacy.get('status')!='PASS' or compact.get('status')!='PASS':raise RuntimeError('Both native runs must PASS')
    if len(legacy['cases'])!=15 or len(compact['cases'])!=15:raise RuntimeError('Incomplete native case matrix')
    result={'status':'PASS','cases':[],'timings_are_small_scene_samples':True,'undo_strokes_tested':False}
    for a,b in zip(legacy['cases'],compact['cases']):
        if (a['kind'],a['mode'],a['old_level'])!=(b['kind'],b['mode'],b['old_level']):raise RuntimeError('Case matrix mismatch')
        if a['metadata']!=b['metadata'] or a['mask']!=b['mask'] or a['face_sets']!=b['face_sets']:raise RuntimeError('Metadata/mask/Face Sets differ')
        if len(a['variants'])!=4 or len(b['variants'])!=4:raise RuntimeError('Incomplete isolated-layer checks')
        largest=0.
        for label in ('before','retained','high','reloaded'):
            for key in ('points','colors'):largest=max(largest,error(a[label][key],b[label][key]))
        for x,y in zip(a['variants'],b['variants']):
            largest=max(largest,error(x['points'],y['points']),error(x['colors'],y['colors']))
        for run in (a,b):
            for key in ('points','colors'):
                largest=max(largest,error(run['variants'][-1][key],run['reloaded'][key]))
            if run['mode']=='CATMULL_CLARK':
                largest=max(largest,error(run['before']['points'],run['retained']['points']))
        if largest>1e-5:raise RuntimeError(f'Geometry/color regression {a["kind"]}/{a["mode"]}: {largest}')
        result['cases'].append({'kind':a['kind'],'mode':a['mode'],'max_error':largest,
                                'legacy_seconds':a['seconds'],'compact_seconds':b['seconds']})
    result['legacy_process_peak_bytes']=legacy.get('process_peak_working_set_bytes')
    result['compact_process_peak_bytes']=compact.get('process_peak_working_set_bytes')
    return result
