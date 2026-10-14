import copy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'kit-polypaint-multi-v01'))
from verify_multi import verify_multi
points=[[0.,0.,0.]];A0=[[.1,.2,.3,1.]];A1=[[.8,.2,.1,1.]];A2=[[.6,.1,.2,1.]];B0=[[1.,1.,1.,1.]];B1=[[.01,.15,.85,1.]]
s={}
def state(labels,colors,levels,active):
 for label in labels.split():s[label]={'label':label,'colors':copy.deepcopy(colors),'levels':dict(levels),'active':active,'layers':[],'points':points[:]}
state('before-a undo-a',{'SkinColor':A0},{'SkinColor':2},'SkinColor')
state('after-a redo-a',{'SkinColor':A1},{'SkinColor':2},'SkinColor')
state('before-b undo-b',{'SkinColor':A1,'Mask':B0},{'SkinColor':2,'Mask':2},'Mask')
state('after-b redo-b select-undo',{'SkinColor':A1,'Mask':B1},{'SkinColor':2,'Mask':2},'Mask')
state('select-a select-redo higher-before promotion-undo',{'SkinColor':A1,'Mask':B1},{'SkinColor':2,'Mask':2},'SkinColor')
state('higher-undo promotion-redo',{'SkinColor':A1,'Mask':B1},{'SkinColor':3,'Mask':2},'SkinColor')
state('higher-after higher-redo boundary-redo rename-undo',{'SkinColor':A2,'Mask':B1},{'SkinColor':3,'Mask':2},'SkinColor')
state('renamed rename-redo remove-undo restored-b add-undo removed-active-c remove-active-redo after-subdivide',{'Albedo':A2,'Mask':B1},{'Albedo':3,'Mask':2},'Albedo')
state('removed-b remove-redo',{'Albedo':A2},{'Albedo':3},'Albedo')
state('added-c add-redo remove-active-undo',{'Albedo':A2,'Mask':B1,'Roughness':B0},{'Albedo':3,'Mask':2,'Roughness':0},'Roughness')
r={'status':'PASS','snapshots':list(s.values())};verify_multi(r)
def reject(label,mutate):
 trial=copy.deepcopy(r);items={v['label']:v for v in trial['snapshots']};mutate(items)
 try:verify_multi(trial)
 except (RuntimeError,KeyError):return
 raise AssertionError('False PASS: '+label)
reject('cross-attribute contamination',lambda x:x['after-b']['colors']['SkinColor'][0].__setitem__(0,.5))
reject('other master promoted',lambda x:x['higher-after']['levels'].__setitem__('Mask',3))
reject('selection undo wrong',lambda x:x['select-undo'].__setitem__('active','SkinColor'))
reject('paint did not modify color',lambda x:x['after-a'].__setitem__('colors',copy.deepcopy(x['before-a']['colors'])))
reject('paint modifies geometry',lambda x:x['higher-after'].__setitem__('points',[[.1,0.,0.]]))
reject('removed attribute remains',lambda x:x['removed-b']['colors'].__setitem__('Mask',B1))
reject('promotion undo keeps fine master',lambda x:x['promotion-undo']['levels'].__setitem__('SkinColor',3))
reject('subdivide changes color master',lambda x:x['after-subdivide']['levels'].__setitem__('Albedo',4))
print('PASS verifier valid fixture and 8 fault-injection cases')
