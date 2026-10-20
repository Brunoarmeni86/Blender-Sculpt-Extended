# SPDX-License-Identifier: GPL-2.0-or-later
"""Targeted native Subdivide test. All scenes belong to this test directory."""
import bpy
import json
import os
from pathlib import Path
import time
import traceback
import numpy as np

ROOT = Path(os.environ['SUBDIV_TEST_ROOT'])
PHASE = os.environ['SUBDIV_TEST_PHASE']
REF = Path(os.environ['SUBDIV_REFERENCE']) if os.environ.get('SUBDIV_REFERENCE') else None
S = dict(states={}, timings_ms=[], max_errors={})
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.edit.use_global_undo = True


def ctx():
    win = bpy.context.window
    area = next(a for a in win.screen.areas if a.type == 'VIEW_3D')
    return bpy.context.temp_override(window=win, area=area,
        region=next(r for r in area.regions if r.type == 'WINDOW'))


def mod():
    return next(m for m in bpy.context.object.modifiers if m.type == 'MULTIRES')


def probe(action):
    path = ROOT / 'temporary-probe.json'
    with ctx():
        assert bpy.ops.sculpt.polypaint_blur_probe_v01(action=action, filepath=str(path)) == {'FINISHED'}
    data = json.loads(path.read_text())
    path.unlink()
    return data


def points():
    array = np.asarray(probe('EXPORT')['positions'], dtype=np.float32)
    assert array.ndim == 2 and array.shape[1] == 3 and np.isfinite(array).all()
    return array


DOMAIN_KEYS = ['colors_hash', 'base_color_hash', 'mask_hash', 'face_sets_hash',
               'protection_hash', 'sample_count', 'level']


def geometry_equal(label, actual, expected):
    assert actual.shape == expected.shape, (label, 'Different evaluated layout')
    tolerance = 2e-6 * max(1., float(np.max(np.abs(expected))))
    error = float(np.max(np.abs(actual - expected)))
    S['max_errors'][label] = dict(max_abs=error, tolerance=tolerance)
    assert error <= tolerance, (label, 'Geometry differs', error, tolerance)


def capture(label):
    data = probe('SUMMARY')
    value = {key: data[key] for key in DOMAIN_KEYS + ['layer_hash', 'positions_hash']}
    m = mod()
    value.update(total=m.total_levels, sculpt=m.sculpt_levels, active=m.active_sculpt_layer_index,
        layers=[[l.name, l.strength, l.visible] for l in m.sculpt_layers])
    actual = points()
    S['states'][label] = value
    # Always retain local points for Undo/Redo/reopen checks, independent of the
    # cross-profile comparison. Hashes may differ for dense vs implicit zero.
    np.savez_compressed(ROOT / (label + '.npz'), positions=actual)
    if REF:
        expected = json.loads((REF / (PHASE + '.json')).read_text())['states'][label]
        for key in DOMAIN_KEYS + ['total', 'sculpt', 'active', 'layers']:
            assert value[key] == expected[key], (label, 'Domain/metadata differs', key)
        with np.load(REF / (label + '.npz')) as archive:
            geometry_equal('reference_' + label, actual, archive['positions'])
        changed = [k for k in ['layer_hash', 'positions_hash'] if value[k] != expected[k]]
        if changed:
            S.setdefault('representation_hash_differences', {})[label] = changed
    with ctx():
        assert bpy.ops.sculpt.multires_base_fingerprint(label=label,
            include_layer_payloads=True, include_surface=True) == {'FINISHED'}
    print('CAPTURE ' + label, flush=True)
    return actual


def same_local(label, actual_label, expected_label):
    a, b = S['states'][actual_label], S['states'][expected_label]
    for key in DOMAIN_KEYS + ['total', 'sculpt', 'active', 'layers', 'layer_hash']:
        assert a[key] == b[key], (label, 'Stored state not restored', key)
    with np.load(ROOT / (actual_label + '.npz')) as actual, np.load(ROOT / (expected_label + '.npz')) as expected:
        geometry_equal(label, actual['positions'], expected['positions'])


def activate_draw():
    assert bpy.ops.brush.asset_activate(asset_library_type='ESSENTIALS',
        relative_asset_identifier='brushes/essentials_brushes-mesh_sculpt.blend/Brush/Draw') == {'FINISHED'}
    brush = bpy.context.tool_settings.sculpt.brush
    unified = getattr(bpy.context.tool_settings.sculpt, 'unified_paint_settings', None)
    if unified:
        unified.use_unified_size = unified.use_unified_strength = False
    # This source version uses diameters, not radii. Fix the world-space
    # footprint explicitly so the small fixture receives enough samples.
    brush.use_locked_size = 'SCENE'
    brush.size = 96
    brush.unprojected_size = .6
    brush.strength = .55
    brush.use_pressure_size = brush.use_pressure_strength = False
    brush.stroke_method = 'SPACE'
    brush.spacing = 10
    brush.input_samples = 1
    brush.jitter = 0.
    brush.use_smooth_stroke = False
    for name in ['use_sculpt_uniform_accumulation', 'use_sculpt_adaptive_spacing',
                 'use_sculpt_adaptive_stabilization']:
        if hasattr(brush, name):
            setattr(brush, name, False)
    # Fixture preparation must be independent of asset automasking defaults.
    for owner in [brush, bpy.context.tool_settings.sculpt]:
        for prop in owner.bl_rna.properties:
            if prop.identifier.startswith('use_automasking_') and prop.type == 'BOOLEAN' and not prop.is_readonly:
                setattr(owner, prop.identifier, False)
    print('DRAW_SETTINGS ' + json.dumps(dict(size=brush.size,
        size_unit=brush.use_locked_size, world_diameter=brush.unprojected_size,
        strength=brush.strength, stroke=brush.stroke_method)), flush=True)


def stroke(dx, replay=False):
    from bpy_extras import view3d_utils
    before = probe('SUMMARY') if PHASE == 'seed' else None
    with ctx():
        reg, rv = bpy.context.region, bpy.context.space_data.region_3d
        samples = []
        for i, offset in enumerate([-24, -12, 0, 12, 24]):
            mouse = (reg.width / 2 + dx + offset, reg.height / 2)
            origin = view3d_utils.region_2d_to_origin_3d(reg, rv, mouse)
            direction = view3d_utils.region_2d_to_vector_3d(reg, rv, mouse)
            hit, location, *_ = bpy.context.scene.ray_cast(bpy.context.evaluated_depsgraph_get(), origin, direction)
            assert hit, 'Stroke missed fixture'
            samples.append(dict(name='SubdivideTest', location=tuple(location), mouse=mouse,
                mouse_event=mouse, pressure=1., size=float(bpy.context.tool_settings.sculpt.brush.size), x_tilt=0., y_tilt=0.,
                time=i * .1, is_start=i == 0))
        if replay:
            if REF:
                recorded = json.loads((REF / 'exercise.json').read_text())['post_subdivide_stroke']
                assert recorded['region'] == [reg.width, reg.height]
                samples = recorded['samples']
            S['post_subdivide_stroke'] = dict(region=[reg.width, reg.height], samples=samples)
        assert bpy.ops.sculpt.brush_stroke('EXEC_DEFAULT', True,
            stroke=samples, override_location=True) == {'FINISHED'}
        if before:
            after_draw = probe('SUMMARY')
        assert bpy.ops.sculpt.multires_layer_commit() == {'FINISHED'}
        mod().sculpt_layer_recording = False
    bpy.context.view_layer.update()
    if before:
        after_commit = probe('SUMMARY')
        diagnostic = dict(dx=dx, before=before['positions_hash'],
            after_draw=after_draw['positions_hash'], after_commit=after_commit['positions_hash'],
            layer_before=before['layer_hash'], layer_after=after_commit['layer_hash'], samples=samples)
        S.setdefault('stroke_diagnostics', []).append(diagnostic)
        print('SEED_STROKE ' + json.dumps(diagnostic), flush=True)


def seed_detail(label, dx):
    before = points()
    layer_before = probe('SUMMARY')['layer_hash']
    stroke(dx)
    yield .5
    after = points()
    delta = float(np.max(np.abs(after - before)))
    S.setdefault('fixture_detail', {})[label] = dict(max_abs=delta, threshold=1e-5)
    print('FIXTURE_DETAIL %s max_abs=%.9g' % (label, delta), flush=True)
    assert delta > 1e-5, ('Fixture needs actual recorded detail', label, delta)
    assert probe('SUMMARY')['layer_hash'] != layer_before, ('Fixture needs a changed layer payload', label)


def audit_ui():
    from bl_ui import space_view3d_sculpt_extended as ui
    from types import SimpleNamespace
    calls = []
    class Properties:
        def __init__(self, rna):
            object.__setattr__(self, 'rna', rna)
        def __setattr__(self, name, value):
            prop = self.rna.properties[name]
            if prop.type == 'ENUM':
                assert value in prop.enum_items.keys(), (name, value)
            calls.append(['operator_property', name, str(value)])
    class Layout:
        def __getattr__(self, name):
            if name in {'row', 'column', 'box', 'split'}:
                return lambda *args, **kwargs: Layout()
            if name in {'label', 'separator'}:
                return lambda *args, **kwargs: None
            raise AttributeError(name)
        def prop(self, owner, name, **kwargs):
            assert hasattr(owner, name), ('Missing native UI property', name)
            calls.append(['prop', name])
        def operator(self, name, **kwargs):
            category, op = name.split('.')
            rna = getattr(getattr(bpy.ops, category), op).get_rna_type()
            calls.append(['operator', name])
            return Properties(rna)
    with ctx():
        assert ui.VIEW3D_PT_sculpt_extended_multires.poll(bpy.context)
        ui.VIEW3D_PT_sculpt_extended_multires.draw(SimpleNamespace(layout=Layout()), bpy.context)
        assert bpy.ops.sculpt.extended_prepare_transport('EXEC_DEFAULT') == {'FINISHED'}
        brush = bpy.context.tool_settings.sculpt.brush
        assert brush.sculpt_draw_transport_v01 and brush.stroke_method == 'ANCHORED'
        assert brush.falloff_shape == 'SPHERE' and not brush.use_edge_to_edge
        assert os.environ.get('BLENDER_SCULPT_DRAW_TRANSPORT_V01') == '1'
        activate_draw()
        brush.sculpt_draw_transport_v01 = False
    assert len(calls) > 20
    S['ui_audit'] = dict(status='PASS_NATIVE_RNA_AND_PANEL_CALLBACK', calls=calls,
        limitation='Validates panel draw callback and actual RNA/operator availability; visual layout requires manual check.')


def setup():
    with ctx():
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)
        bpy.ops.mesh.primitive_cube_add(size=2)
        obj = bpy.context.object
        obj.name = 'Subdivide_Layers_Test'
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.mesh.subdivide(number_cuts=15 if os.environ.get('BAKE_L0') == '1' else 7)
        bpy.ops.object.mode_set(mode='OBJECT')
        # Curved, asymmetric cage: cached original detail must be nontrivial.
        import math
        for vertex in obj.data.vertices:
            q = vertex.co.normalized()
            radius = 1. + .07 * math.sin(3. * q.x) * math.cos(2. * q.y)
            vertex.co = q * radius
        crease = obj.data.attributes.new(name='crease_vert', type='FLOAT', domain='POINT')
        for index, item in enumerate(crease.data):
            item.value = .35 if index % 47 == 0 else 0.
        attr = obj.data.color_attributes.new(name='KeepColor', type='FLOAT_COLOR', domain='CORNER')
        obj.data.color_attributes.active_color = attr
        for i, item in enumerate(attr.data):
            item.color = (.15 + (i % 13) / 20., .3, .6, 1.)
        faces = obj.data.attributes.new(name='.sculpt_face_set', type='INT', domain='FACE')
        for i, item in enumerate(faces.data):
            item.value = 1 + (i // 16) % 3
        m = obj.modifiers.new(name='Multires', type='MULTIRES')
        for _ in range(0 if os.environ.get('BAKE_L0') == '1' else 3):
            assert bpy.ops.object.multires_subdivide(modifier=m.name, mode='CATMULL_CLARK') == {'FINISHED'}
        m.levels = m.sculpt_levels = m.total_levels
        obj.data.use_mirror_x = obj.data.use_mirror_y = obj.data.use_mirror_z = False
        bpy.ops.object.mode_set(mode='SCULPT')
        bpy.ops.view3d.view_axis(type='TOP', align_active=False)
        bpy.context.space_data.region_3d.view_distance = 4
        activate_draw()
        assert bpy.ops.sculpt.multires_layer_add() == {'FINISHED'}
        m.sculpt_layers[0].name = 'Detail_A'
        m.sculpt_layer_recording = True
    yield .8
    yield from seed_detail('Detail_A', -40)
    with ctx():
        mod().sculpt_layers[0].strength = .65
        assert bpy.ops.sculpt.multires_layer_add() == {'FINISHED'}
        mod().sculpt_layers[1].name = 'Detail_B_Hidden'
        mod().sculpt_layer_recording = True
    yield .4
    yield from seed_detail('Detail_B', 40)
    with ctx():
        mod().sculpt_layers[1].strength = -.4
        mod().sculpt_layers[1].visible = False
        for name in ['Empty_Visible', 'Empty_Hidden']:
            assert bpy.ops.sculpt.multires_layer_add() == {'FINISHED'}
            mod().sculpt_layers[-1].name = name
        mod().sculpt_layers[3].visible = False
        mod().active_sculpt_layer_index = 0
        mod().sculpt_layer_recording = False
    bpy.context.view_layer.update()
    yield .4
    capture('seed')
    audit_ui()
    with ctx():
        assert bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / 'seed.blend')) == {'FINISHED'}
    finish('PASS_SEED')


def full_visibility(visible):
    # Full graph evaluation is an independent reference to incremental toggles.
    with ctx():
        bpy.ops.object.mode_set(mode='OBJECT')
        for layer, state in zip(mod().sculpt_layers, visible):
            layer.visible = state
        bpy.ops.object.mode_set(mode='SCULPT')
    bpy.context.view_layer.update()


def subdivide(label, mode='CATMULL_CLARK'):
    old = mod().total_levels
    with ctx():
        start = time.perf_counter()
        assert bpy.ops.object.multires_subdivide(modifier=mod().name, mode=mode) == {'FINISHED'}
        elapsed = (time.perf_counter() - start) * 1000
        assert mod().total_levels == old + 1
        mod().levels = mod().sculpt_levels = mod().total_levels
    bpy.context.view_layer.update()
    S['timings_ms'].append(dict(label=label, mode=mode, from_level=old,
        to_level=mod().total_levels, native_operator_ms=elapsed))


def exercise():
    # Establish two references before baking: visible-only and combined detail.
    visible = capture('visible_before')
    payload_before = probe('SUMMARY')['layer_hash']
    full_visibility([True, True, True, False])
    yield .4
    combined = points().copy()
    assert float(np.max(np.abs(combined - visible))) > 1e-5, 'Hidden fixture must contain real detail'
    full_visibility([True, False, True, False])
    yield .4
    # Select the hidden layer: bake must retain the selected hidden target.
    with ctx():
        mod().active_sculpt_layer_index = 1
        bpy.ops.ed.undo_push(message='Before targeted Bake Visible test')
    before = capture('before_bake')
    with ctx():
        assert bpy.ops.sculpt.multires_base_capture('EXEC_DEFAULT', True) == {'FINISHED'}
    yield .5
    after = capture('after_bake')
    geometry_equal('bake_visible_surface', after, before)
    assert len(mod().sculpt_layers) == 4
    assert not mod().sculpt_layers[1].visible
    assert mod().active_sculpt_layer_index == 1
    assert probe('SUMMARY')['layer_hash'] != payload_before, 'Visible offsets should be cleared'
    for key in DOMAIN_KEYS:
        assert S['states']['after_bake'][key] == S['states']['before_bake'][key], ('Unrelated domain modified', key)
    with ctx():
        assert bpy.ops.ed.undo() == {'FINISHED'}
    yield .6
    capture('undo')
    same_local('undo_bake', 'undo', 'before_bake')
    with ctx():
        assert bpy.ops.ed.redo() == {'FINISHED'}
    yield .6
    capture('redo')
    same_local('redo_bake', 'redo', 'after_bake')
    # Revealing the previously hidden detail must match the pre-bake combination.
    full_visibility([True, True, True, False])
    yield .4
    geometry_equal('hidden_detail_retained', points(), combined)
    full_visibility([False, False, False, False])
    yield .4
    native_after = points().copy()
    geometry_equal('visible_baked_into_native_base', native_after, visible)
    with ctx():
        try:
            result = bpy.ops.sculpt.multires_base_capture('EXEC_DEFAULT', True)
        except RuntimeError as exc:
            assert 'Cannot bake:' in str(exc), str(exc)
            result = {'CANCELLED'}
        assert result == {'CANCELLED'}, ('All-hidden bake must cancel', result)
    yield .3
    geometry_equal('all_hidden_cancel_preserves_base', points(), native_after)
    full_visibility([True, False, True, False])
    yield .3
    with ctx():
        assert bpy.ops.sculpt.multires_base_capture('EXEC_DEFAULT', True) == {'FINISHED'}
    yield .3
    geometry_equal('repeat_bake_no_double_application', points(), visible)
    full_visibility([True, True, True, False])
    yield .3
    capture('saved')
    with ctx():
        assert bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / 'candidate.blend')) == {'FINISHED'}
    finish('PASS_BAKE_VISIBLE_HIDDEN')


def reopen():
    capture('reopen')
    expected = json.loads((ROOT / 'exercise.json').read_text())['states']['saved']
    actual = S['states']['reopen']
    for key in DOMAIN_KEYS + ['layer_hash', 'total', 'sculpt', 'active', 'layers']:
        assert actual[key] == expected[key], ('Reopen changed stored state', key)
    with np.load(ROOT / 'saved.npz') as data:
        geometry_equal('reopen_saved', points(), data['positions'])
    finish('PASS_REOPEN')


def finish(status):
    S['status'] = status
    (ROOT / (PHASE + '.json')).write_text(json.dumps(S, indent=2))
    bpy.ops.wm.quit_blender()


def pipeline():
    if PHASE == 'seed':
        yield from setup()
    else:
        source = ROOT / 'candidate.blend' if PHASE == 'reopen' else Path(os.environ['SUBDIV_SEED'])
        bpy.ops.wm.open_mainfile(filepath=str(source))
        yield 1.5
        if PHASE == 'reopen':
            reopen()
        else:
            yield from exercise()


steps = pipeline()


def tick():
    try:
        return next(steps)
    except StopIteration:
        return None
    except Exception:
        S['status'] = 'FAIL'
        S['error'] = traceback.format_exc()
        print(S['error'], flush=True)
        (ROOT / (PHASE + '.json')).write_text(json.dumps(S, indent=2))
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(tick, first_interval=2., persistent=True)
