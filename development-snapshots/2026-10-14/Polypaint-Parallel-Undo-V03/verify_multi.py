"""Pure verifier shared by the native runner and fixture-based local checks."""
def distance(a,b):
    if len(a)!=len(b):raise RuntimeError('Sample count differs')
    return max((abs(x-y) for aa,bb in zip(a,b) for x,y in zip(aa,bb)),default=0.)

def verify_multi(report):
    if report.get('status')!='PASS':raise RuntimeError('Snapshot read incomplete')
    s={x['label']:x for x in report['snapshots']};checked=[]
    def same(a,b,rename=None,attributes=None,geometry=True):
        aa=s[a]['colors'];bb=s[b]['colors']
        if rename:aa={rename.get(k,k):v for k,v in aa.items()}
        names=set(aa) if attributes is None else set(attributes)
        if attributes is None and names!=set(bb):raise RuntimeError('Attribute set differs '+a+'/'+b)
        for name in names:
            if distance(aa[name],bb[name])>1e-5:raise RuntimeError('Color changed '+a+'/'+b+'/'+name)
        if s[a]['layers']!=s[b]['layers']:raise RuntimeError('Shape layers changed '+a+'/'+b)
        if geometry and distance(s[a]['points'],s[b]['points'])>1e-6:raise RuntimeError('Geometry changed '+a+'/'+b)
        checked.append((a,b))
    for a,b in [('before-a','undo-a'),('after-a','redo-a'),('before-b','undo-b'),('after-b','redo-b'),
                ('redo-b','select-a'),('redo-b','select-undo'),('select-a','select-redo'),
                ('higher-before','higher-undo'),('higher-before','promotion-undo'),
                ('higher-before','promotion-redo'),('higher-after','higher-redo'),
                ('higher-after','boundary-redo'),('boundary-redo','rename-undo'),
                ('renamed','rename-redo'),('rename-redo','remove-undo'),
                ('removed-b','remove-redo'),('rename-redo','restored-b'),
                ('restored-b','add-undo'),('added-c','add-redo'),
                ('added-c','remove-active-undo'),('removed-active-c','remove-active-redo')]:same(a,b)
    same('boundary-redo','renamed',rename={'SkinColor':'Albedo'})
    same('redo-a','before-b',attributes=['SkinColor'])
    same('before-b','after-b',attributes=['SkinColor'])
    same('higher-before','higher-after',attributes=['Mask'])
    same('restored-b','added-c',attributes=['Albedo','Mask'])
    same('rename-redo','removed-b',attributes=['Albedo'])
    same('added-c','removed-active-c',attributes=['Albedo','Mask'])
    same('remove-active-redo','after-subdivide',geometry=False)
    for a,b,name in [('before-a','after-a','SkinColor'),('before-b','after-b','Mask'),('higher-before','higher-after','SkinColor')]:
        if distance(s[a]['colors'][name],s[b]['colors'][name])<1e-6:raise RuntimeError('Paint did not change '+name)
        if distance(s[a]['points'],s[b]['points'])>1e-6:raise RuntimeError('Paint changed geometry')
    for label in ('after-b','higher-before'):
        if s[label]['levels'].get('SkinColor')!=2 or s[label]['levels'].get('Mask')!=2:raise RuntimeError('Independent initial levels incorrect')
    for label in ('higher-after','higher-redo','promotion-redo','boundary-redo'):
        if s[label]['levels'].get('SkinColor')!=3 or s[label]['levels'].get('Mask')!=2:raise RuntimeError('Promotion changed another master')
    if s['promotion-undo']['levels'].get('SkinColor')!=2:raise RuntimeError('Promotion Undo did not restore resolution')
    if s['select-undo']['active']!='Mask' or s['select-redo']['active']!='SkinColor':raise RuntimeError('Selection Undo/Redo failed')
    if 'Mask' in s['removed-b']['colors'] or 'Roughness' in s['removed-active-c']['colors']:raise RuntimeError('Attribute removal failed')
    if s['renamed']['levels'].get('Albedo')!=3 or 'SkinColor' in s['renamed']['levels']:raise RuntimeError('Master rename failed')
    if s['after-subdivide']['levels'].get('Albedo')!=3 or s['after-subdivide']['levels'].get('Mask')!=2:raise RuntimeError('Geometry changed master resolution')
    return {'status':'PASS','preservation_pairs':checked,'independent_master_levels':True,
            'paint_undo_redo':True,'selection_rename_remove_add_undo_redo':True,'reload_all_attributes':True}
