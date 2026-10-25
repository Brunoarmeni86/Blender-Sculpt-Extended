from pathlib import Path
import hashlib
import json
import os
import traceback

common=Path(__file__).with_name('smooth_routing_regression.py')
exec(common.read_text().split('\nsteps = run()')[0])
phase='l0_isolation'
original_run=run
results=[]

class DispView(ctypes.Structure):
    _fields_=[('totdisp',ctypes.c_int),('level',ctypes.c_int),
              ('disps',ctypes.c_void_p),('hidden',ctypes.c_void_p)]

def payload(layer):
    raw=LayerView.from_address(layer.as_pointer())
    assert raw.name.decode()==layer.name and raw.strength==layer.strength
    h=hashlib.sha256()
    if raw.base_deltas and raw.base_verts_num:
        h.update(ctypes.string_at(raw.base_deltas,raw.base_verts_num*3*4))
    if raw.disps:
        grids=(DispView*raw.grids_num).from_address(raw.disps)
        for grid in grids:
            assert 0<=grid.totdisp<=1048576
            h.update(str((grid.totdisp,grid.level,bool(grid.disps))).encode())
            if grid.disps:
                h.update(ctypes.string_at(grid.disps,grid.totdisp*3*4))
    return h.hexdigest()

def smooth_settings(name='Smooth',preserve=True):
    b=brush(name,preserve=preserve)
    b.curve_distance_falloff_preset='CONSTANT'
    b.unprojected_size=12
    b.use_smooth_shape_preserve=False
    return b

def test_run():
    # Reuse only the real L0 fixture and propagation checks, not the old guarded cases.
    gen=original_run()
    while not results:
        delay=next(gen)
        yield delay
    gen.close()
    with context():
        modifier().sculpt_layers[0].strength=.4
        assert bpy.ops.sculpt.multires_layer_add()=={'FINISHED'}
        modifier().sculpt_layers[1].name='Protected high detail'
    b=brush('Draw',preserve=False)
    b.unprojected_size=.8
    b.strength=.8
    before_detail=positions()
    stroke()
    assert np.max(np.abs(positions()-before_detail))>1e-5
    with context():
        modifier().active_sculpt_layer_index=0
        seed=root/'l0-isolation-with-other-detail.blend'
        bpy.ops.wm.save_as_mainfile(filepath=str(seed),check_existing=False)
    yield .4
    changes={}
    cases=['isolated_without_other','isolated_with_other','shift_with_other','native_with_other']
    if os.environ.get('SMOOTH_AUDIT_UNDO_CONTROL')=='1':
        cases=['native_with_other']
    for case in cases:
        with context(): bpy.ops.wm.open_mainfile(filepath=str(seed))
        yield .3
        with context():
            wanted=case!='isolated_without_other'
            if modifier().sculpt_layers[1].visible!=wanted:
                modifier().sculpt_layers[1].visible=wanted
        yield .2
        b=smooth_settings(preserve=case!='native_with_other')
        if case=='shift_with_other':
            b=smooth_settings('Draw',preserve=True)
        before=positions()
        base_before=base_data()
        protected_before=payload(modifier().sculpt_layers[1])
        active_payload_before=payload(modifier().sculpt_layers[0])
        stroke('SMOOTH' if case=='shift_with_other' else 'None',commit=False)
        yield .2
        after=positions()
        change=after-before
        maximum=float(np.max(np.abs(change)))
        assert np.isfinite(after).all() and maximum>1e-6,(case,'No actual smoothing',maximum)
        assert np.array_equal(base_data(),base_before),(case,'L0 payload changed')
        assert payload(modifier().sculpt_layers[1])==protected_before,(case,'Other layer payload changed')
        changes[case]=change
        results.append(dict(case=case,status='PASS',max_change=maximum,protected_payload_unchanged=True))
        save_result()
        if case=='isolated_with_other' or os.environ.get('SMOOTH_AUDIT_UNDO_CONTROL')=='1':
            with context(): assert bpy.ops.ed.undo()=={'FINISHED'}
            yield .3
            undo_points=positions()
            undo_error=float(np.max(np.abs(undo_points-before)))
            (root/'l0-undo-diagnostic.json').write_text(json.dumps({'max_error':undo_error,
                'active_payload_before':active_payload_before,
                'active_payload_undo':payload(modifier().sculpt_layers[0]),
                'protected_payload_before':protected_before,
                'active':modifier().active_sculpt_layer_index,
                'layers':[[l.name,l.strength,l.visible,payload(l)] for l in modifier().sculpt_layers],
                'base_max':float(np.max(np.abs(base_data())))},indent=2))
            np.savez(root/'l0-undo-diagnostic.npz',before=before,after=after,undo=undo_points)
            assert undo_error<2e-6,('Isolated Smooth Undo mismatch',undo_error)
            with context(): assert bpy.ops.ed.redo()=={'FINISHED'}
            yield .3
            assert np.max(np.abs(positions()-after))<2e-6,'Isolated Smooth Redo mismatch'
            with context(): assert bpy.ops.sculpt.multires_layer_commit()=={'FINISHED'}
            yield .2
            assert np.max(np.abs(positions()-after))<2e-6,'Commit changed the smoothed surface'
            saved=root/'isolated-l0-smooth-save.blend'
            with context():
                bpy.ops.wm.save_as_mainfile(filepath=str(saved),check_existing=False)
                bpy.ops.wm.open_mainfile(filepath=str(saved))
            yield .4
            assert np.max(np.abs(positions()-after))<2e-6,'Isolated Smooth reopen mismatch'
            assert np.array_equal(base_data(),base_before)
            assert payload(modifier().sculpt_layers[1])==protected_before
            results.append(dict(case='isolated_l0_undo_redo_reopen',status='PASS'))
            save_result()
    if os.environ.get('SMOOTH_AUDIT_UNDO_CONTROL')=='1':
        with context(): bpy.ops.wm.quit_blender()
        return
    isolated_error=float(np.max(np.abs(changes['isolated_with_other']-changes['isolated_without_other'])))
    native_difference=float(np.max(np.abs(changes['native_with_other']-changes['isolated_with_other'])))
    assert isolated_error<2e-5,('Other visible detail influenced isolated smoothing',isolated_error)
    assert native_difference>1e-5,('Preserve ON merely used native Smooth',native_difference)
    results.append(dict(case='reference_independent_of_other_detail',status='PASS',
                        isolated_change_difference=isolated_error,native_change_difference=native_difference))
    save_result()
    with context(): bpy.ops.wm.quit_blender()

steps=test_run()
def tick():
    try:return next(steps)
    except StopIteration:return None
    except Exception:
        error=traceback.format_exc()
        (root/'l0-isolation-ERROR.txt').write_text(error)
        print(error,flush=True)
        os._exit(1)
bpy.app.timers.register(tick,first_interval=1,persistent=True)
