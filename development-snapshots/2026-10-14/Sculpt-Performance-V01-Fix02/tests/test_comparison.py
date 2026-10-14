import copy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent.parent))
from confronta_ab import compare
sample={'points':[(1.,2.,3.)],'colors':[(.2,.3,.4,1.)]}
row={'kind':'empty','mode':'CATMULL_CLARK','old_level':2,'metadata':[('L',1.,True)],'mask':[0.],'face_sets':[1],
     'before':sample,'retained':sample,'high':sample,'reloaded':sample,'variants':[sample]*4,'seconds':.01}
a={'status':'PASS','cases':[copy.deepcopy(row) for _ in range(15)]}
assert compare(a,copy.deepcopy(a))['status']=='PASS'
mutations=[lambda b:b['cases'].pop(),lambda b:b['cases'][0]['variants'].pop(),
 lambda b:b['cases'][0].update(old_level=3),lambda b:b['cases'][0].update(metadata=[]),
 lambda b:b['cases'][0].update(mask=[.2]),lambda b:b['cases'][0].update(face_sets=[2]),
 lambda b:b['cases'][0]['high'].update(points=[(1.,2.,3.01)]),
 lambda b:b['cases'][0]['high'].update(colors=[(.2,.3,.5,1.)]),
 lambda b:b['cases'][0]['high'].update(points=[(1.,2.)]),
 lambda b:b['cases'][0]['high'].update(colors=[(float('nan'),.3,.4,1.)]),
 lambda b:b['cases'][0]['high'].update(points=[(float('inf'),2.,3.)])]
for mutate in mutations:
 b=copy.deepcopy(a);mutate(b)
 try:compare(a,b)
 except RuntimeError:pass
 else:raise AssertionError('Regression accepted')
print('PASS comparator: matching data and 11 regression/missing/non-finite cases')
