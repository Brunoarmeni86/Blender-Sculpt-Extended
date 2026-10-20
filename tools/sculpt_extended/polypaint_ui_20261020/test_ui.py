"""Focused Polypaint UI checks. Run with Python, or Blender --background --python."""
import importlib.util
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace as NS
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'scripts/startup/bl_ui/space_view3d_sculpt_extended.py'
try:
    import bpy as native_bpy
except ImportError:
    native_bpy = None


def load_ui(bpy_module, name):
    spec = importlib.util.spec_from_file_location(name, SOURCE)
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {'bpy': bpy_module}):
        spec.loader.exec_module(module)
    return module


fake_bpy = NS(types=NS(Panel=object, Operator=object))
ui = load_ui(fake_bpy, 'polypaint_ui_under_test')


class Layout:
    def __init__(self, events=None, operator_context='INVOKE_DEFAULT'):
        self.events = [] if events is None else events
        self.operator_context = operator_context

    def column(self, **kwargs):
        return Layout(self.events, self.operator_context)

    def row(self, **kwargs):
        return Layout(self.events, self.operator_context)

    def label(self, **kwargs):
        self.events.append(('label', kwargs))

    def prop(self, owner, name, **kwargs):
        # Missing RNA properties must fail, rather than being silently accepted.
        getattr(owner, name)
        self.events.append(('prop', name, kwargs))

    def operator(self, name, **kwargs):
        props = NS()
        self.events.append(('operator', name, self.operator_context, kwargs, props))
        return props


class Attributes(list):
    def __contains__(self, item):
        return any(a.name == item for a in self) if isinstance(item, str) else super().__contains__(item)


def context(mode='SCULPT', kind='PAINT', names=('Color',), masters=(), selected='Color'):
    brush = None if kind is None else NS(
        sculpt_brush_type=kind, strength=.5, size=50, blend='MIX', flow=1.,
        wet_mix=0., wet_persistence=0., density=1., smear_deform_type='DRAG')
    modifier = NS(type='MULTIRES', polypaint_attribute=selected,
                  polypaint_attributes=[NS(name=n) for n in masters])
    obj = NS(type='MESH', mode=mode, modifiers=[modifier],
             data=NS(color_attributes=Attributes(NS(name=n) for n in names)))
    return NS(object=obj, tool_settings=NS(sculpt=NS(brush=brush)))


def draw(panel, ctx):
    layout = Layout()
    panel.draw(NS(layout=layout), ctx)
    return layout.events


class BrushControls(unittest.TestCase):
    def check_brush(self, kind, expected):
        ctx = context(kind=kind)
        events = draw(ui.VIEW3D_PT_multires_polypaint, ctx)
        self.assertEqual(events, draw(ui.VIEW3D_PT_polypaint_blur_v01, ctx))
        self.assertEqual([e[1] for e in events if e[0] == 'prop'], expected)
        ops = [e for e in events if e[0] == 'operator']
        self.assertEqual([e[4].action for e in ops], ['PAINT', 'BLUR', 'SMEAR'])
        self.assertTrue(all(e[1] == 'sculpt.polypaint_blur_probe_v01' and
                            e[2] == 'EXEC_REGION_WIN' for e in ops))

    def test_paint(self):
        self.check_brush('PAINT', ['strength', 'size', 'blend', 'flow', 'wet_mix',
                                  'wet_persistence', 'density'])

    def test_blur(self):
        self.check_brush('BLUR', ['strength', 'size'])

    def test_smear(self):
        self.check_brush('SMEAR', ['strength', 'size', 'smear_deform_type'])

    def test_no_brush(self):
        self.check_brush(None, [])


class AttributeControls(unittest.TestCase):
    def events(self, **kwargs):
        ctx = context(**kwargs)
        events = draw(ui.VIEW3D_PT_multires_polypaint_attributes, ctx)
        self.assertEqual(events, draw(ui.VIEW3D_PT_color_v03_attributes, ctx))
        return events

    def test_select_and_remove(self):
        events = self.events(names=('B', 'A'), selected='B')
        ops = [e for e in events if e[0] == 'operator' and
               e[1] == 'sculpt.multires_polypaint_attribute']
        self.assertEqual([(e[4].action, e[4].name) for e in ops],
                         [('SELECT', 'A'), ('REMOVE', 'A'), ('SELECT', 'B'), ('REMOVE', 'B')])
        self.assertEqual([e[3]['depress'] for e in ops if e[4].action == 'SELECT'], [False, True])
        self.assertTrue(all(e[2] == 'EXEC_REGION_WIN' for e in ops))

    def test_residual_master_can_be_removed_but_not_selected(self):
        events = self.events(masters=('Orphan', 'Color'), selected='Missing')
        ops = [e for e in events if e[0] == 'operator' and
               e[1] == 'sculpt.multires_polypaint_attribute']
        self.assertEqual([(e[4].action, e[4].name) for e in ops],
                         [('SELECT', 'Color'), ('REMOVE', 'Color'),
                          ('REMOVE', 'Missing'), ('REMOVE', 'Orphan')])

    def test_empty_still_offers_confirmed_clear(self):
        events = self.events(names=(), selected='')
        self.assertIn(('label', {'text': 'Nessun Color Attribute.'}), events)
        clear = [e for e in events if e[0] == 'operator']
        self.assertEqual(len(clear), 1)
        self.assertEqual(clear[0][1:3], ('sculpt.color_v03_clear_confirm', 'INVOKE_REGION_WIN'))

    def test_object_mode(self):
        self.events(mode='OBJECT')

    def test_clear_keeps_native_undo_call(self):
        called = []
        ops = NS(sculpt=NS(multires_polypaint_attribute=lambda *a, **kw:
                          called.append((a, kw)) or {'FINISHED'}))
        with patch.object(ui, 'bpy', NS(ops=ops)):
            self.assertEqual(ui.SCULPT_OT_color_v03_clear_confirm.execute(NS(), context()), {'FINISHED'})
        self.assertEqual(called, [(('EXEC_DEFAULT', True), {'action': 'CLEAR'})])


class Eligibility(unittest.TestCase):
    def test_modes(self):
        for mode in ('SCULPT', 'OBJECT', 'EDIT'):
            with self.subTest(mode=mode):
                ctx = context(mode=mode)
                self.assertEqual(ui.VIEW3D_PT_multires_polypaint.poll(ctx), mode == 'SCULPT')
                self.assertEqual(ui.VIEW3D_PT_multires_polypaint_attributes.poll(ctx),
                                 mode in {'SCULPT', 'OBJECT'})

    def test_missing_or_nonmesh_object(self):
        for obj in (None, NS(type='CURVE')):
            ctx = NS(object=obj)
            self.assertFalse(ui.VIEW3D_PT_multires_polypaint.poll(ctx))
            self.assertFalse(ui.VIEW3D_PT_multires_polypaint_attributes.poll(ctx))

    def test_absent_or_ambiguous_multires(self):
        for count in (0, 2):
            ctx = context()
            ctx.object.modifiers *= count
            self.assertFalse(ui.VIEW3D_PT_multires_polypaint.poll(ctx))
            self.assertFalse(ui.VIEW3D_PT_multires_polypaint_attributes.poll(ctx))

    def test_registration_list_and_categories(self):
        for panel in (ui.VIEW3D_PT_multires_polypaint, ui.VIEW3D_PT_multires_polypaint_attributes):
            self.assertIn(panel, ui.classes)
            self.assertEqual((panel.bl_category, panel.bl_space_type, panel.bl_region_type),
                             ('Multires', 'VIEW_3D', 'UI'))
        self.assertEqual(ui.VIEW3D_PT_facesets_rc1.bl_category, 'Sculpt Extended')


@unittest.skipIf(native_bpy is None, 'Run inside Blender for actual RNA/class lifecycle checks')
class BlenderRegistration(unittest.TestCase):
    def test_register_poll_and_unregister(self):
        module = load_ui(native_bpy, 'polypaint_ui_native_probe')
        registered = []
        had_props = hasattr(native_bpy.types.Scene, 'facesets_rc2_id')
        try:
            for cls in module.classes:
                existing = getattr(native_bpy.types, cls.__name__, None)
                if existing is None:
                    native_bpy.utils.register_class(cls)
                    registered.append(cls)
                elif cls.bl_idname.startswith('VIEW3D_PT_multires_polypaint'):
                    self.assertEqual(existing.bl_category, 'Multires')
            if not had_props:
                module.register_props()
            self.assertTrue(hasattr(native_bpy.types.Scene, 'facesets_rc2_id'))
            native_bpy.ops.mesh.primitive_cube_add()
            obj = native_bpy.context.object
            try:
                obj.modifiers.new('UI probe', 'MULTIRES')
                self.assertTrue(module.VIEW3D_PT_multires_polypaint_attributes.poll(native_bpy.context))
                self.assertFalse(module.VIEW3D_PT_multires_polypaint.poll(native_bpy.context))
                for name in ('strength', 'size', 'blend', 'flow', 'wet_mix',
                             'wet_persistence', 'density', 'smear_deform_type'):
                    self.assertIn(name, native_bpy.types.Brush.bl_rna.properties)
            finally:
                native_bpy.data.objects.remove(obj, do_unlink=True)
        finally:
            if not had_props:
                module.unregister_props()
            for cls in reversed(registered):
                native_bpy.utils.unregister_class(cls)
        self.assertEqual(hasattr(native_bpy.types.Scene, 'facesets_rc2_id'), had_props)


if __name__ == '__main__':
    # Blender arguments do not belong to unittest.
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    if not result.wasSuccessful():
        raise SystemExit(1)

