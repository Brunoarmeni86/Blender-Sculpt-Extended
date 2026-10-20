import bpy,json,os,traceback,math
from pathlib import Path
# This process receives no activation flags from the launcher.
expected=json.loads((Path(__file__).parent/'PROFILE.json').read_text())
for key,value in expected.items():
    if key!='BLENDER_SCULPT_POLYPAINT_L0_TRACE':
        assert os.environ.get(key)==value,('Missing native default',key,os.environ.get(key))
assert os.environ.get('BLENDER_SCULPT_EXTENDED_NATIVE')=='1','Native initializer did not run'
assert 'bl_ui.space_view3d_sculpt_extended' in __import__('sys').modules,'Integrated UI was not loaded'
for name in ['facesets_rc2_id','facesets_rc10_solidify','facesets_rc10_limit']:
    assert name in bpy.types.Scene.bl_rna.properties,('Integrated UI property missing',name)
for name in ['VIEW3D_PT_facesets_rc1','VIEW3D_PT_polypaint_blur_v01','VIEW3D_PT_color_v03_attributes']:
    assert bpy.types.Panel.bl_rna_get_subclass_py(name),('Integrated panel missing',name)
print('[NATIVE_RELEASE] PASS defaults and integrated UI without external launcher',flush=True)
ROOT=Path(os.environ['SUITE_CASE_ROOT']);logs=ROOT/'logs';logs.mkdir(parents=True,exist_ok=True)
case=os.environ['SUITE_CASE'];phase=os.environ['FACESETS_RC8_PHASE'];S={};checks=[]
result=logs/('result-'+phase+'.json');reference=logs/'reference.json'
bpy.context.preferences.use_preferences_save=False
bpy.context.preferences.view.show_splash=False

def ctx():
    win=bpy.context.window;area=next(a for a in win.screen.areas if a.type=='VIEW_3D')
    return bpy.context.temp_override(window=win,area=area,region=next(r for r in area.regions if r.type=='WINDOW'))
def ob():return bpy.context.object
def mod():return next(m for m in ob().modifiers if m.type=='MULTIRES')
def mode(value):
    with ctx():
        if ob().mode!=value:assert bpy.ops.object.mode_set(mode=value)=={'FINISHED'}
    bpy.context.view_layer.update()
def snapshot():
    p=logs/'snapshot.json'
    with ctx():assert bpy.ops.sculpt.polypaint_blur_probe_v01(action=('SUMMARY' if case in ['large','native_remove','budget'] else 'EXPORT'),filepath=str(p))=={'FINISHED'}
    value=json.loads(p.read_text())
    if S.get('last_snapshot') is not None:
        (logs/'stage-previous.json').write_text(json.dumps(S['last_snapshot']))
    (logs/'stage-latest.json').write_text(json.dumps(value))
    S['last_snapshot']=value
    assert value.get("blur_backend")==4,"The executable does not contain Color V04"
    return value
def state(d):return {k:v for k,v in d.items() if k not in ['blur_dabs','smear_dabs','paint_dabs','budget_aborts','wet_samples','wet_fallbacks','wet_reuses']}
def colors(d):return {k:d[k] for k in ['level','master_level','present','colors','colors_hash']}
def invariants(d,before):
    for key in ['positions','positions_hash','hidden','mask','mask_hash','face_sets_hash','layer_hash','protection_hash']:
        assert d[key]==before[key],('Separate domain changed',key)
def check(label):checks.append(label);print('[BLUR_V01] PASS',label,flush=True)
def activate(action):
    with ctx():assert bpy.ops.sculpt.polypaint_blur_probe_v01(action=action)=={'FINISHED'}
    brush=bpy.context.tool_settings.sculpt.brush
    ups=getattr(bpy.context.tool_settings.sculpt,'unified_paint_settings',None)
    if ups:
        ups.use_unified_strength=False;ups.use_unified_size=False;ups.use_unified_color=False
    brush.strength=.65;brush.size=110;brush.use_pressure_strength=False;brush.use_pressure_size=False
    brush.stroke_method='SPACE';brush.spacing=10
    brush.flow=1;brush.density=1;brush.wet_mix=0;brush.wet_persistence=0;brush.blend='MIX'
    for prop in ['use_flow_pressure','use_density_pressure','use_wet_mix_pressure','use_wet_persistence_pressure']:
        setattr(brush,prop,False)
    brush.wet_paint_radius_factor=.25
    return brush

def setup():
    if case=="budget":os.environ["BLENDER_SCULPT_POLYPAINT_WORK_MB"]="512"
    with ctx():
        bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj=ob();obj.name='Polypaint_Blur_V01_Test'
        # Subdivide the base once so Face Set/hidden tests have adequate base topology.
        if case not in ['large','budget','l0_display']:
            bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT');bpy.ops.mesh.subdivide(number_cuts=(31 if case=="cage_boundary" else 1));bpy.ops.object.mode_set(mode='OBJECT')
        mesh=obj.data
        keep=mesh.attributes.new(name='KeepScalar',type='FLOAT',domain='POINT')
        for v in keep.data:v.value=123.0
        attr=mesh.color_attributes.new(name='BlurColor',type='FLOAT_COLOR',domain='CORNER')
        mesh.color_attributes.active_color=attr
        for value in attr.data:value.color=(1,1,1,1)
        m=obj.modifiers.new(name='Multires',type='MULTIRES')
        for i in range(9 if case=='large' else (8 if case=='budget' else (1 if case=='cage_boundary' else 4))):assert bpy.ops.object.multires_subdivide(modifier=m.name,mode='CATMULL_CLARK')=={'FINISHED'}
        m.levels=m.sculpt_levels=m.total_levels
        obj.data.use_mirror_x=obj.data.use_mirror_y=obj.data.use_mirror_z=False
        bpy.ops.object.mode_set(mode='SCULPT')
        bpy.ops.view3d.view_axis(type='TOP',align_active=False)
        bpy.context.space_data.region_3d.view_distance=4
        bpy.context.space_data.shading.type='SOLID';bpy.context.space_data.shading.color_type='VERTEX'
        # Color operations must work with an existing geometric layer and REC off.
        assert bpy.ops.sculpt.multires_layer_add()=={'FINISHED'}
        m.sculpt_layer_recording=False
    S['object']=ob().name
    brush=activate('PAINT');brush.color=(.03,.05,.1);brush.flow=1;brush.wet_mix=0;brush.density=1;brush.size=70;brush.strength=1
    S['white']=snapshot()

def stroke(smooth=False):
    from bpy_extras import view3d_utils
    with ctx():
        reg=bpy.context.region;rv=bpy.context.space_data.region_3d;samples=[]
        path=S.get('stroke_path') or [(reg.width/2+dx,reg.height/2) for dx in [-36,-24,-12,0,12,24,36]]
        for i,mouse in enumerate(path):
            origin=view3d_utils.region_2d_to_origin_3d(reg,rv,mouse)
            direction=view3d_utils.region_2d_to_vector_3d(reg,rv,mouse)
            hit,location,normal,index,obj,matrix=bpy.context.scene.ray_cast(bpy.context.evaluated_depsgraph_get(),origin,direction)
            assert hit,'Generated mesh ray missed'
            samples.append(dict(name='PolypaintBlurV01',location=tuple(location),mouse=mouse,mouse_event=mouse,pressure=1.,size=float(bpy.context.tool_settings.sculpt.brush.size),x_tilt=0.,y_tilt=0.,time=i*.1,is_start=i==0))
        kw=dict(stroke=samples,override_location=True)
        if smooth:kw['brush_toggle']='SMOOTH'
        return bpy.ops.sculpt.brush_stroke('EXEC_DEFAULT',True,**kw)
def seed():
    if case=='l0_display':
        # Broad native stroke: the six-face cage averages a large surface.
        bpy.context.tool_settings.sculpt.brush.size=260
        for _ in range(3):assert stroke()=={'FINISHED'}
    else:assert stroke()=={'FINISHED'}
    d=snapshot();assert d['colors_hash']!=S['white']['colors_hash'],'Harness seed Paint did not change actual CCG colors'
    assert d['nonuniform'],'Seed must have a visible color boundary'
    assert d['master_level']==mod().total_levels and d['level']==mod().total_levels
    if case=='large':assert d['sample_count']>1048576,'Dense fixture does not exceed old global cap'
    check('Native Paint created a real nonuniform HR color master; geometric layer present, REC off')
def prepare():
    if case=='shift':brush=activate('PAINT')
    elif case.startswith('smear') or case in ['mask','hidden','lower']:
        brush=activate('SMEAR')
        brush.smear_deform_type={'smear_pinch':'PINCH','smear_expand':'EXPAND'}.get(case,'DRAG')
    else:brush=activate('BLUR')
    if case=='budget':brush.size=600
    if case=='large':brush.size=45
    if case=='mask':
        with ctx():assert bpy.ops.paint.mask_flood_fill('EXEC_DEFAULT',True,mode='VALUE',value=1)=={'FINISHED'}
    if case=='hidden':
        with ctx():
            reg=bpy.context.region;cx,cy=reg.width//2,reg.height//2
            assert bpy.ops.sculpt.face_set_box_gesture('EXEC_DEFAULT',True,xmin=cx+10,xmax=cx+55,ymin=cy-45,ymax=cy+45)=={'FINISHED'}
            # RC5 uses the next unused ID on a previously neutral mesh.
            assert bpy.ops.sculpt.face_set_change_visibility('EXEC_DEFAULT',True,mode='HIDE_ACTIVE',active_face_set=2)=={'FINISHED'}
        assert snapshot()['hidden'],'Hidden fixture empty'
    S['before']=snapshot()
    (logs/'stage-before-checkpoint.json').write_text(json.dumps(S['before']))
    with ctx():assert bpy.ops.ed.undo_push(message='Before Blur V01')=={'FINISHED'}
    after_checkpoint=snapshot()
    (logs/'stage-after-checkpoint.json').write_text(json.dumps(after_checkpoint))
    stage_diff=[key for key in S['before'] if key not in ['blur_dabs','smear_dabs','paint_dabs','budget_aborts','wet_samples','wet_fallbacks','wet_reuses'] and S['before'][key]!=after_checkpoint[key]]
    (logs/'checkpoint-diff.json').write_text(json.dumps({'changed_fields':stage_diff}))
    print('[BLUR_V01] checkpoint changed fields',stage_diff,flush=True)
def blur():
    assert stroke(case=='shift')=={'FINISHED'}
    S['after']=snapshot();before=S['before'];after=S['after']
    (logs/'stage-after-stroke.json').write_text(json.dumps(after))
    if case in ['blur','shift','large']:assert after['blur_dabs']>before['blur_dabs'],'Stroke did not enter the HR Blur backend'
    if case.startswith('smear'):assert after['smear_dabs']>before['smear_dabs'],'Stroke did not enter HR Smear'
    if case!='budget':assert after['budget_aborts']==before['budget_aborts'],'Unexpected local memory budget rejection'
    invariants(after,before)
    assert all(a[3]==b[3] for a,b in zip(before['colors'],after['colors'])),'Blur changed alpha'
    if case=='budget':
        assert after['budget_aborts']>before['budget_aborts'],'Fixture did not exercise work-budget abort'
        assert state(after)==state(before),'Budget-aborted dab published partial changes'
    elif case=='mask':
        assert colors(after)==colors(before),'Fully masked Blur modified color storage'
    elif case=='hidden':
        assert all(after['colors'][i]==before['colors'][i] for i in before['hidden']),'Blur modified hidden colors'
    else:
        assert after['colors_hash']!=before['colors_hash'],'Color brush had no measurable effect'
        assert all(math.isfinite(x) for c in after['colors'] for x in c)
    check('Local color brush checks passed: effect or expected protection/budget behavior; geometry, Face Sets, masks, alpha and layer data retained')
def undo():
    with ctx():assert bpy.ops.ed.undo()=={'FINISHED'}
def redo():
    with ctx():assert bpy.ops.ed.redo()=={'FINISHED'}
def undo_check():assert state(snapshot())==state(S['before']),'Blur Undo differs from exact source snapshot'
def redo_check():
    assert state(snapshot())==state(S['after']),'Blur Redo differs'
    check('Native Color Undo/Redo exact, including sparse grid presence')
def lower_guard():
    S['high']=snapshot();mod().levels=mod().sculpt_levels=2;bpy.context.view_layer.update()
    try:status=stroke()
    except RuntimeError:status={'CANCELLED'}
    assert status=={'CANCELLED'},('Expected explicit lower-level rejection',status)
    mod().levels=mod().sculpt_levels=4;bpy.context.view_layer.update()
    assert state(snapshot())==state(S['high']),'Rejected low-level Blur changed retained master or other domains'
    check('Lower-level Blur rejected; complete L4 master and geometry preserved')
def ownership():
    m=mod();mesh=ob().data
    return dict(mesh_colors=sorted(a.name for a in mesh.color_attributes),active=m.polypaint_attribute,
        level=m.polypaint_level,archives=sorted([a.name,a.level] for a in m.polypaint_attributes),
        scalar=[v.value for v in mesh.attributes['KeepScalar'].data])
def attribute_op(action,name=''):
    with ctx():assert bpy.ops.sculpt.multires_polypaint_attribute('EXEC_DEFAULT',True,action=action,name=name)=={'FINISHED'}
    bpy.context.view_layer.update()
def no_name(name):
    data=ownership();assert name not in data['mesh_colors'] and data['active']!=name and all(x[0]!=name for x in data['archives']),data
def empty_colors():
    d=ownership();assert d['mesh_colors']==[] and not d['active'] and d['level']==0 and d['archives']==[],d
    assert all(v==123 for v in d['scalar'])
    d=snapshot();assert not any(d['present']),'Residual active HR grids after complete removal'
    invariants(d,S['attribute_A'])
def attrs_prepare():
    S['attribute_A']=snapshot();S['attribute_A_owner']=ownership()
    attribute_op('ADD','ColorB')
    brush=activate('PAINT');brush.color=(.8,.08,.03)
    assert stroke()=={'FINISHED'}
    S['attribute_B']=snapshot()
    attribute_op('SELECT','BlurColor')
    assert state(snapshot())==state(S['attribute_A'])
    S['AB_owner']=ownership()
def attrs_remove_B():
    attribute_op('REMOVE','ColorB');no_name('ColorB')
    assert state(snapshot())==state(S['attribute_A'])
    S['removed_B_owner']=ownership()
def attrs_undo_B():
    undo();assert ownership()==S['AB_owner'] and state(snapshot())==state(S['attribute_A'])
def attrs_redo_B():
    redo();assert ownership()==S['removed_B_owner'] and state(snapshot())==state(S['attribute_A'])
    check('Inactive painted attribute removed completely; other colors retained; native Undo/Redo exact')
def attrs_orphan():
    attribute_op('ADD','ColorB');attribute_op('SELECT','BlurColor')
    ob().data.color_attributes.remove(ob().data.color_attributes['ColorB'])
    bpy.context.view_layer.update()
    assert any(x[0]=='ColorB' for x in ownership()['archives']),'Orphan fixture did not retain its named master'
    attribute_op('REMOVE','ColorB');no_name('ColorB')
    assert state(snapshot())==state(S['attribute_A'])
    check('Orphaned named master after native mesh attribute removal can be deleted')
def attrs_last():
    attribute_op('REMOVE','BlurColor');empty_colors()
def attrs_undo_last():
    undo();assert state(snapshot())==state(S['attribute_A'])
def attrs_redo_last():
    redo();empty_colors()
    check('Last Color Attribute and active master removed; native Undo/Redo restores exact HR colors')
def attrs_reuse_clear():
    attribute_op('ADD','BlurColor')
    fresh=snapshot();assert fresh['colors_hash']!=S['attribute_A']['colors_hash'],'Reused name resurrected old paint'
    attribute_op('ADD','ColorB')
    S['before_clear']=state(snapshot());S['before_clear_owner']=ownership()
    attribute_op('CLEAR');empty_colors()
def attrs_undo_clear():
    undo();assert state(snapshot())==S['before_clear'] and ownership()==S['before_clear_owner']
def attrs_redo_clear():
    redo();empty_colors()
    check('Explicit Clear All Colors removes all active/inactive masters; Undo/Redo and same-name reuse correct')
def save():
    d=snapshot();reference.write_text(json.dumps({'snapshot':state(d),'object':ob().name,'ownership':ownership()}))
    with ctx():assert bpy.ops.wm.save_as_mainfile(filepath=str(logs/'case.blend'))=={'FINISHED'}
    check('Saved independent small-mesh test')
def reopen():
    ref=json.loads(reference.read_text());obj=bpy.data.objects[ref['object']]
    bpy.context.view_layer.objects.active=obj;obj.select_set(True);mode('SCULPT')
    assert ownership()==ref['ownership'],'Reopened color master ownership differs'
    assert state(snapshot())==ref['snapshot'],'Reopened colors/master/geometry or protected domains differ'
    check('Reopened exact HR colors, geometry, Face Sets and geometric layer data')
def done():
    result.write_text(json.dumps({'status':'PASS','case':case,'phase':phase,'checks':checks,'scope':'V04 independent meshes. Paint, Blur, Smear L0-L4; delta residual; Paint controls and available blend modes; protection; native attribute removal; budget; dense master; Undo/Redo and persistence. See checks for actual covered variants.'},indent=2))
    bpy.app.timers.register(lambda:bpy.ops.wm.quit_blender() and None,first_interval=.3)

def set_level(level):
    # Rebuild SculptSession explicitly: do not compare stale CCG buffers with an L0 Mesh PBVH.
    mode('OBJECT')
    mod().levels=mod().sculpt_levels=level
    bpy.context.view_layer.update()
    mode('SCULPT')

def check_scalar():
    assert all(v.value==123.0 for v in ob().data.attributes['KeepScalar'].data),'Unrelated scalar changed'

def residual(before,after,level):
    if level>=after['master_level'] or not before['colors']:return
    h=after['master_side'];low=2 if level==0 else (1<<(level-1))+1
    ratio=(h-1)//(low-1);area=h*h
    assert (h-1)%(low-1)==0
    max_error=0.0
    # Check retained high frequency, using the actual coarse-knot changes.
    # Only unmasked cases use this invariant; protection intentionally modulates the field.
    for start in range(0,len(before['colors']),area):
        for y in range(h):
            ly=min(y//ratio,low-2);fy=(y-ly*ratio)/ratio
            for x in range(h):
                lx=min(x//ratio,low-2);fx=(x-lx*ratio)/ratio
                indices=[start+ly*ratio*h+lx*ratio,start+ly*ratio*h+(lx+1)*ratio,
                         start+(ly+1)*ratio*h+lx*ratio,start+(ly+1)*ratio*h+(lx+1)*ratio]
                weights=[(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy]
                index=start+y*h+x
                for c in range(3):
                    expected=sum((after['colors'][i][c]-before['colors'][i][c])*w for i,w in zip(indices,weights))
                    actual=after['colors'][index][c]-before['colors'][index][c]
                    max_error=max(max_error,abs(expected-actual))
    assert max_error<2e-6,('High-frequency residual was flattened',level,max_error)
    check('L%d: fine residual preserved, max interpolation error %.3g'%(level,max_error))

def begin_level(level):
    set_level(level)
    action={'paint_levels':'PAINT','blur_levels':'BLUR','smear_levels':'SMEAR','cage_boundary':'PAINT'}[case]
    brush=activate(action);brush.size=230;brush.strength=.5
    if action=='PAINT':
        brush.color=(.7,.15,.25);brush.wet_mix=.3;brush.wet_persistence=.65;brush.density=.8
    if action=='SMEAR':brush.smear_deform_type=['DRAG','PINCH','EXPAND'][level%3]
    with ctx():assert bpy.ops.ed.undo_push(message='Before V04 L%d'%level)=={'FINISHED'}
    S['before']=snapshot();S['test_level']=level;S['test_action']=action

def run_level():
    assert stroke()=={'FINISHED'}
    before=S['before'];after=S['after']=snapshot()
    invariants(after,before);check_scalar()
    counter={'PAINT':'paint_dabs','BLUR':'blur_dabs','SMEAR':'smear_dabs'}[S['test_action']]
    assert after[counter]>before[counter],('Backend not entered',counter,S['test_level'])
    assert after['budget_aborts']==before['budget_aborts'],'Unexpected work-budget abort'
    assert after['colors_hash']!=before['colors_hash'],('No actual color effect',case,S['test_level'])
    if S['test_action']!='PAINT':assert all(a[3]==b[3] for a,b in zip(before['colors'],after['colors'])),'Blur/Smear changed alpha'
    residual(before,after,S['test_level'])
    check('%s L%d changes actual master; separate domains retained'%(S['test_action'],S['test_level']))

def level_undo():
    undo();assert state(snapshot())==state(S['before']),('Undo differs',case,S.get('test_level'),S.get('variant'))

def level_redo():
    redo();assert state(snapshot())==state(S['after']),('Redo differs',case,S.get('test_level'),S.get('variant'))
    check('Exact native Undo/Redo including master presence and L0 base color')

def choose_wet_path(data):
    # Ensure Wet Persistence sees a changing input: cross two visible coarse
    # samples with different retained colors, instead of a short stationary patch.
    from bpy_extras import view3d_utils
    from mathutils import Vector
    high=data['master_side'];low=(1<<(mod().sculpt_levels-1))+1
    ratio=(high-1)//(low-1);area=low*low
    visible=[];seen=set()
    with ctx():
        reg=bpy.context.region;rv=bpy.context.space_data.region_3d
        max_z=max(p[2] for p in data['positions'])
        for index,raw in enumerate(data['positions']):
            if raw[2]<max_z*.65:continue
            world=ob().matrix_world@Vector(raw)
            pixel=view3d_utils.location_3d_to_region_2d(reg,rv,world)
            if pixel is None:continue
            key=(round(pixel.x,4),round(pixel.y,4))
            if key in seen:continue
            origin=view3d_utils.region_2d_to_origin_3d(reg,rv,pixel)
            direction=view3d_utils.region_2d_to_vector_3d(reg,rv,pixel)
            hit,location,*_=bpy.context.scene.ray_cast(bpy.context.evaluated_depsgraph_get(),origin,direction)
            if not hit or (location-world).length>.02:continue
            seen.add(key)
            grid,j=divmod(index,area);y,x=divmod(j,low)
            color=data['colors'][grid*high*high+y*ratio*high+x*ratio]
            visible.append((sum(color[:3])/3,tuple(pixel),color[:3]))
    assert len(visible)>=2,'Wet fixture lacks visible coarse samples'
    dark=min(visible,key=lambda v:v[0]);light=max(visible,key=lambda v:v[0])
    assert light[0]-dark[0]>.05,'Wet fixture lacks a measurable source-color boundary'
    S['stroke_path']=[tuple(dark[1][c]*(1-t/6)+light[1][c]*(t/6) for c in range(2)) for t in range(7)]
    (logs/'stage-wet-path.json').write_text(json.dumps(dict(dark=dark,light=light,mouse=S['stroke_path'])))
    check('Wet fixture crosses two visible coarse samples with distinct source colors')

def options_init():
    set_level(2);brush=activate('PAINT');brush.color=(.8,.1,.3);brush.size=190;brush.strength=.5
    with ctx():assert bpy.ops.ed.undo_push(message='Paint controls V04 baseline')=={'FINISHED'}
    S['option_base']=snapshot();S['option_results']={}
    choose_wet_path(S['option_base'])
    enum=brush.bl_rna.properties['blend'].enum_items
    variants=[{'name':'flow_zero','flow':0},{'name':'density_zero','density':0},
        {'name':'dry'},{'name':'density_low','density':.2},
        {'name':'wet','wet_mix':1},{'name':'wet_persistent','wet_mix':1,'wet_persistence':.9}]
    variants += [dict(name='blend_'+item.identifier,blend=item.identifier) for item in enum if item.identifier]
    tasks=[]
    for variant in variants:
        tasks.extend([lambda v=variant:option_prepare(v),option_stroke,option_undo])
    steps[0:0]=tasks

def option_prepare(variant):
    assert state(snapshot())==state(S['option_base']),'Options must start from identical source'
    brush=bpy.context.tool_settings.sculpt.brush
    for name,value in dict(flow=1,density=1,wet_mix=0,wet_persistence=0,blend='MIX').items():setattr(brush,name,value)
    for name,value in variant.items():
        if name!='name':setattr(brush,name,value)
    S['variant']=variant['name'];S['before']=snapshot()
    assert not brush.use_wet_persistence_pressure
    (logs/'stage-paint-controls.json').write_text(json.dumps(dict(variant=S['variant'],wet_mix=brush.wet_mix,wet_persistence=brush.wet_persistence,wet_radius=brush.wet_paint_radius_factor,pressure_override=brush.use_wet_persistence_pressure)))

def option_stroke():
    assert stroke()=={'FINISHED'}
    before=S['before'];after=S['after']=snapshot();variant=S['variant']
    invariants(after,before);check_scalar()
    assert after['paint_dabs']>before['paint_dabs'],'Paint V04 not called'
    assert after['budget_aborts']==before['budget_aborts'],'Unexpected abort'
    if variant in ['wet','wet_persistent']:
        assert after['wet_samples']>before['wet_samples'],'Wet sampling not executed'
    if variant=='wet_persistent':assert after['wet_reuses']>before['wet_reuses'],'Wet history never reused across dabs'
    if variant in ['flow_zero','density_zero']:assert colors(after)==colors(before),'Zero control modified storage'
    elif variant in ['dry','density_low','wet','wet_persistent']:assert after['colors_hash']!=before['colors_hash'],('Control had no effect',variant)
    # Some modes on a particular source are legitimately identity operations.
    assert all(math.isfinite(v) for row in after['colors'] for v in row)
    residual(before,after,2)
    S['option_results'][variant]=after['colors_hash']
    check('Paint control / blend '+variant)

def option_undo():
    undo();assert state(snapshot())==state(S['before']),('Option Undo differs',S['variant'])

def options_compare():
    r=S['option_results']
    (logs/'stage-options-results.json').write_text(json.dumps(r,indent=2))
    assert r['density_low']!=r['dry'],'Density has no measurable influence'
    assert r['wet']!=r['dry'],'Wet Mix has no measurable influence'
    assert r['wet_persistent']!=r['wet'],'Wet Persistence has no measurable influence across dabs'
    check('Density, Wet Mix and Wet Persistence produce distinct results from identical source')

def full_mask_init():
    with ctx():assert bpy.ops.paint.mask_flood_fill('EXEC_DEFAULT',True,mode='VALUE',value=1)=={'FINISHED'}

def masked_level(level):
    set_level(level)
    for action in ['PAINT','BLUR','SMEAR']:
        brush=activate(action);brush.size=230;brush.color=(1,0,0)
        before=snapshot()
        if level==0:assert before['hr_mask_full_corners']==len(ob().data.loops),'L0 fixture lost the retained HR mask'
        assert stroke()=={'FINISHED'};after=snapshot()
        invariants(after,before);assert colors(after)==colors(before),('Fully masked color changed',level,action)
        assert after['base_color_hash']==before['base_color_hash'],'Masked base color changed'
    check('Fully masked Paint/Blur/Smear unchanged at L%d'%level)

def hidden_init():
    with ctx():
        reg=bpy.context.region;cx,cy=reg.width//2,reg.height//2
        assert bpy.ops.sculpt.face_set_box_gesture('EXEC_DEFAULT',True,xmin=cx+10,xmax=cx+55,ymin=cy-45,ymax=cy+45)=={'FINISHED'}
        assert bpy.ops.sculpt.face_set_change_visibility('EXEC_DEFAULT',True,mode='HIDE_ACTIVE',active_face_set=2)=={'FINISHED'}
    high=snapshot();assert high['hidden'],'Hidden fixture empty'
    S['protected_indices']=high['hidden']

def hidden_level(level):
    set_level(level);brush=activate('PAINT');brush.size=230;brush.color=(.8,.1,.2)
    before=snapshot();assert stroke()=={'FINISHED'};after=snapshot();invariants(after,before)
    assert all(after['colors'][i]==before['colors'][i] for i in S['protected_indices']),('High hidden color changed',level)
    check('Original high hidden samples retained after Paint L%d'%level)

def budget_prepare():
    os.environ["BLENDER_SCULPT_POLYPAINT_WORK_MB"]="1"
    brush=activate('BLUR');brush.size=600
    S['before']=snapshot()

def budget_stroke():
    assert stroke()=={'FINISHED'}
    after=snapshot();assert after['budget_aborts']>S['before']['budget_aborts'],'Budget fixture did not abort'
    assert state(after)==state(S['before']),'Budget abort published partial storage'
    check('1 MiB budget abort before publication, master and all separate domains exact')

def large_prepare():
    brush=activate('SMEAR');brush.size=45;S['before']=snapshot()
    with ctx():assert bpy.ops.ed.undo_push(message='Before dense V04')=={'FINISHED'}
    S['before']=snapshot()

def large_stroke():
    assert stroke()=={'FINISHED'}
    S['after']=snapshot();invariants(S['after'],S['before'])
    assert S['after']['colors_hash']!=S['before']['colors_hash'],'Dense Smear no effect'
    assert S['after']['budget_aborts']==S['before']['budget_aborts']
    check('Dense master over 1 million samples changes without global sample cap')

def native_prepare():
    # Standard native minus, no Python UI wrapper and no custom REMOVE call.
    S['attribute_A']=snapshot();S['native_owner']=ownership()

def native_remove():
    with ctx():assert bpy.ops.geometry.color_attribute_remove('EXEC_DEFAULT',True)=={'FINISHED'}
    empty_colors();S['native_empty']=snapshot();S['native_empty_owner']=ownership()

def native_undo():
    undo();assert ownership()==S['native_owner'] and state(snapshot())==state(S['attribute_A'])

def native_redo():
    redo();empty_colors();assert state(snapshot())==state(S['native_empty'])
    check('Standard native minus removes last attribute and HR master; Undo/Redo exact')

def native_reuse():
    mesh=ob().data;attr=mesh.color_attributes.new(name='BlurColor',type='FLOAT_COLOR',domain='CORNER')
    for value in attr.data:value.color=(.8,.25,.05,1)
    mesh.color_attributes.active_color=attr;bpy.context.view_layer.update()
    fresh=snapshot();assert fresh['colors_hash']!=S['attribute_A']['colors_hash'],'Native minus resurrected old master on same-name recreation'
    assert not any(fresh['present']),'Recreated name retained old dense grids'
    S['fresh_hash']=fresh['colors_hash'];check('Same-name orange attribute starts clean')

def native_multi_prepare():
    attribute_op('SELECT','ColorB');S['multi_before']=snapshot();S['multi_owner']=ownership()

def native_multi_remove():
    with ctx():assert bpy.ops.geometry.color_attribute_remove('EXEC_DEFAULT',True)=={'FINISHED'}
    no_name('ColorB');assert state(snapshot())==state(S['attribute_A'])
    S['multi_after']=snapshot();S['multi_after_owner']=ownership()

def native_multi_undo():
    undo();assert state(snapshot())==state(S['multi_before']) and ownership()==S['multi_owner']

def native_multi_redo():
    redo();assert state(snapshot())==state(S['multi_after']) and ownership()==S['multi_after_owner']
    check('Standard minus removes one named master; other painted attribute retained; exact Undo/Redo')

def viewport_display_prepare():
    set_level(0)
    with ctx():
        space=bpy.context.space_data
        space.overlay.show_overlays=False
        space.shading.light='FLAT';space.shading.color_type='VERTEX'
        space.shading.show_shadows=False;space.shading.show_cavity=False
    S['display_data']=state(snapshot())
    assert S['display_data']['level']==0
    assert len(S['display_data']['l0_preview'])==len(ob().data.loops)
    assert any(min(v[:3])<.98 for v in S['display_data']['l0_preview']),'Fixture must retain visible HR color away from the L0 vertices'
    os.environ['BLENDER_SCULPT_POLYPAINT_L0_DRAW_V04D']='0'
    rebuild_display()
    check('L0 fixture uses a six-face cube: HR color remains while legacy base attribute is unchanged')

def save_viewport_png(path,data):
    import numpy as np,zlib,struct
    rgb=np.rint(np.clip(np.flipud(data[:,:,:3]),0,1)*255).astype(np.uint8)
    height,width,_=rgb.shape
    def chunk(tag,payload):return struct.pack('>I',len(payload))+tag+payload+struct.pack('>I',zlib.crc32(tag+payload)&0xffffffff)
    raw=b''.join(b'\x00'+row.tobytes() for row in rgb)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))

def capture_viewport(label):
    # Read the actual window screenshot, not the POST_PIXEL intermediate framebuffer.
    import numpy as np
    with ctx():
        area=bpy.context.area;region=bpy.context.region
        x,y=region.x,region.y
        width,height=region.width,region.height
        bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP',iterations=2)
    path=logs/('window-'+label+'.png')
    assert bpy.ops.screen.screenshot(filepath=str(path))=={'FINISHED'}
    image=bpy.data.images.load(str(path),check_existing=False)
    try:
        iw,ih=image.size
        pixels=np.empty(iw*ih*4,dtype=np.float32)
        image.pixels.foreach_get(pixels)
        full=pixels.reshape((ih,iw,4))
        assert x+width<=iw and y+height<=ih,('Screenshot region bounds',iw,ih,x,y,width,height)
        rw,rh=min(256,width),min(256,height)
        rx,ry=x+(width-rw)//2,y+(height-rh)//2
        data=full[ry:ry+rh,rx:rx+rw,:].copy()
        assert np.isfinite(data).all(),'Nonfinite screenshot pixels'
        assert float(data[:,:,:3].max())>.05,'Screenshot ROI is empty/black; renderer comparison invalid'
        S['viewport_'+label]=data
        save_viewport_png(logs/('viewport-'+label+'.png'),data)
    finally:bpy.data.images.remove(image)

def capture_finish():
    assert state(snapshot())==S['display_data'],'Viewport drawing modified color master/base, geometry or protected domains'

def rebuild_display():
    # Each capture starts with new Sculpt draw data; env changes alone do not tag VBOs.
    mode('OBJECT');mode('SCULPT')
    with ctx():bpy.context.area.tag_redraw()

def viewport_display_enable():
    os.environ['BLENDER_SCULPT_POLYPAINT_L0_DRAW_V04D']='1'
    rebuild_display()

def viewport_display_check():
    import numpy as np
    off=S['viewport_off'];on=S['viewport_on']
    assert off.shape==on.shape,'Viewport size changed between captures'
    delta=np.max(np.abs(on[:,:,:3]-off[:,:,:3]),axis=2)
    blue_shift=(on[:,:,2]-on[:,:,0])-(off[:,:,2]-off[:,:,0])
    blue_pixels=(delta>.02)&(blue_shift>.0001)&(np.mean(on[:,:,:3],axis=2)>.02)
    stats=dict(changed_pixels=int(np.count_nonzero(delta>.02)),blue_tinted_pixels=int(np.count_nonzero(blue_pixels)),maximum_rgb_delta=float(delta.max()),roi_width=int(delta.shape[1]),roi_height=int(delta.shape[0]))
    (logs/'stage-viewport-diff.json').write_text(json.dumps(stats,indent=2))
    assert stats['changed_pixels']>=16,('HR color did not become visible on the actual L0 viewport',stats)
    assert stats['blue_tinted_pixels']>=16,('L0 pixel changes do not show the actual blue source color',stats)
    assert state(snapshot())==S['display_data'],'Display path changed source data'
    check('Actual L0 viewport pixels show HR color; master/base attributes, geometry, Face Sets and layers remain exact')

def l0_display_paint_prepare():
    from bpy_extras import view3d_utils
    from mathutils import Vector
    brush=activate('PAINT');brush.color=(.8,.1,.25);brush.size=230;brush.strength=.6
    # The L0 cube has no vertices at the face centre. Place this verification
    # stroke just inside a visible cage corner, within the native brush radius.
    with ctx():
        region=bpy.context.region;rv=bpy.context.space_data.region_3d
        centre=Vector((region.width/2,region.height/2))
        top=max(v.co.z for v in ob().data.vertices)
        candidates=[]
        for vertex in ob().data.vertices:
            if vertex.co.z<top-1e-6:continue
            screen=view3d_utils.location_3d_to_region_2d(region,rv,ob().matrix_world@vertex.co)
            if screen is not None and 40<screen.x<region.width-40 and 40<screen.y<region.height-40:
                candidates.append((vertex.index,screen))
        assert candidates,'No visible top cage corner available for the L0 paint test'
        index,screen=min(candidates,key=lambda item:(item[1]-centre).length)
        inward=(centre-screen).normalized()
        path=[screen+inward*(20+i*2) for i in range(7)]
        assert all((point-screen).length<brush.size*.25 for point in path),'L0 brush must cover a cage vertex'
        S['stroke_path']=[tuple(point) for point in path]
        (logs/'stage-l0-paint-target.json').write_text(json.dumps(dict(vertex=index,vertex_position=list(ob().data.vertices[index].co),screen=list(screen),radius_pixels=brush.size,path=S['stroke_path'])))
        assert bpy.ops.ed.undo_push(message='L0 paint after HR display')=={'FINISHED'}
    S['before']=snapshot();S['test_level']=0;S['test_action']='PAINT'

if phase=='reopen':steps=[reopen,done]
else:
    steps=[setup]
    steps+=[seed]
    if case in ['paint_levels','blur_levels','smear_levels']:
        for level in [0,1,2,3,4]:steps += [lambda l=level:begin_level(l),run_level,level_undo,level_redo]
    elif case=='l0_display':steps += [viewport_display_prepare,lambda:capture_viewport('off'),capture_finish,viewport_display_enable,lambda:capture_viewport('on'),capture_finish,viewport_display_check,l0_display_paint_prepare,run_level,level_undo,level_redo]
    elif case=='cage_boundary':steps += [lambda:begin_level(0),run_level,level_undo,level_redo]
    elif case=='paint_options':steps += [options_init,options_compare]
    elif case=='mask_levels':steps += [full_mask_init]+[lambda l=level:masked_level(l) for level in [0,1,2,3,4]]
    elif case=='hidden_low':steps += [hidden_init,lambda:hidden_level(2),lambda:hidden_level(0)]
    elif case=='budget':steps += [budget_prepare,budget_stroke]
    elif case=='large':steps += [large_prepare,large_stroke,level_undo,level_redo]
    elif case=='native_remove':steps += [attrs_prepare,native_multi_prepare,native_multi_remove,native_multi_undo,native_multi_redo,native_prepare,native_remove,native_undo,native_redo,native_reuse]
    else:raise RuntimeError('Unknown case '+case)
    steps += [save,done]

def run():
    try:
        if not steps:return None
        action=steps.pop(0);S['step']=action.__name__;print('[POLYPAINT_V04]',case,phase,S['step'],flush=True);action();return .25 if steps else None
    except Exception:
        error=traceback.format_exc();print(error,flush=True)
        if S.get('draw_handle'):
            bpy.types.SpaceView3D.draw_handler_remove(S.pop('draw_handle'),'WINDOW')
        result.write_text(json.dumps({'status':'FAIL','case':case,'phase':phase,'step':S.get('step'),'level':S.get('test_level'),'variant':S.get('variant'),'checks':checks,'error':error},indent=2))
        bpy.app.timers.register(lambda:bpy.ops.wm.quit_blender() and None,first_interval=.3);return None
bpy.app.timers.register(run,first_interval=1.5)
