"""Background smoke check only; does not exercise custom Sculpt Undo or save scenes."""
import bpy
import json
import math
import sys
from pathlib import Path

argv = sys.argv[sys.argv.index('--') + 1:]
output = Path(argv[0])
expected_configuration = argv[1] if len(argv) > 1 else ''


def metadata_text(value):
    if isinstance(value, (bytes, bytearray)):
        return value.decode('utf8', errors='replace').strip()
    return str(value or '').strip()


reported_type = metadata_text(bpy.app.build_type)
report = {
    'version': bpy.app.version_string,
    'version_major_minor': f'{bpy.app.version[0]}.{bpy.app.version[1]}',
    'build_hash': metadata_text(bpy.app.build_hash),
    'build_type': reported_type,
    'expected_build_configuration': expected_configuration,
    'build_type_verified_from_metadata': bool(reported_type),
    'build_type_note': '' if reported_type else 'Build type metadata unavailable; not verified by bpy.app',
    'binary': bpy.app.binary_path,
    'operators': {},
    'test': 'factory-startup/background primitive + standard Multires',
    'custom_sculpt_undo_tested': False,
}
if reported_type and reported_type.lower() != 'release':
    raise RuntimeError(f"Build inattesa: {reported_type}")
if not reported_type:
    print('CLEAN_BUILD_SMOKE: build_type vuoto; continuo i controlli funzionali, tipo non certificato dai metadati')
for name in ('multires_layer_add', 'multires_layer_commit', 'multires_layer_delete'):
    op = getattr(bpy.ops.sculpt, name)
    report['operators'][name] = op.get_rna_type().identifier
bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.object
modifier = obj.modifiers.new(name='Clean Build Smoke Multires', type='MULTIRES')
for _ in range(3):
    result = bpy.ops.object.multires_subdivide(modifier=modifier.name)
    if 'FINISHED' not in result:
        raise RuntimeError(f'Suddivisione fallita: {result}')
modifier.levels = 3
modifier.render_levels = 3
bpy.context.view_layer.update()
evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
mesh = evaluated.to_mesh()
try:
    if len(mesh.vertices) <= len(obj.data.vertices):
        raise RuntimeError('Multires non ha prodotto la geometria attesa')
    if not all(math.isfinite(x) for v in mesh.vertices for x in v.co):
        raise RuntimeError('Coordinate non finite')
    report['base_vertices'] = len(obj.data.vertices)
    report['evaluated_vertices'] = len(mesh.vertices)
    report['multires_levels'] = modifier.total_levels
finally:
    evaluated.to_mesh_clear()
report['status'] = 'PASS'
output.write_text(json.dumps(report, indent=2), encoding='utf8')
print('CLEAN_BUILD_SMOKE PASS: operatori registrati, Multires standard valido; build_type=' + (reported_type or 'non disponibile'))
