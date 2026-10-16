"""Inspect feature registration with factory settings; do not edit or save scene data."""
import json
import runpy
import sys
from pathlib import Path
import bpy

root = Path(__file__).parent
required = [
    'sculpt.multires_layer_add', 'sculpt.multires_layer_commit',
    'sculpt.multires_merge_visible_to_base', 'sculpt.multires_base_fingerprint',
    'sculpt.multires_polypaint_attribute',
]
for name in required:
    namespace, identifier = name.split('.')
    getattr(getattr(bpy.ops, namespace), identifier).get_rna_type()
properties = bpy.types.MultiresModifier.bl_rna.properties
for name in ['sculpt_layers', 'sculpt_layer_recording', 'active_sculpt_layer_index']:
    if properties.get(name) is None:
        raise RuntimeError('Multires RNA missing: ' + name)
if bpy.ops.object.bake_image.get_rna_type().properties.get('polypaint') is None:
    raise RuntimeError('Native Polypaint bake integration missing')
if not getattr(bpy.app.build_options, 'cycles', False):
    raise RuntimeError('Cycles is not included in this build')
runpy.run_path(str(root / 'SculptExtended' / 'experiment-tools' / 'PROVA-POLYPAINT.py'), run_name='consolidation_smoke')
args = sys.argv[sys.argv.index('--') + 1:]
result = {'Success': True, 'Version': bpy.app.version_string,
          'VersionDirectory': '%d.%d' % bpy.app.version[:2],
          'NativeOperators': required, 'RuntimePythonRegistration': True,
          'Scope': 'Registration/startup check; does not test sculpt, Undo, detail preservation or GPU rendering.'}
Path(args[0]).write_text(json.dumps(result, indent=2), encoding='utf-8')
print('[CONSOLIDATED_BUILD_CHECK] ' + json.dumps(result), flush=True)
