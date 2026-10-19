#!/usr/bin/env python3
"""Surgical integration for compact and full-frame caches; preserve other source files."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json

parser = argparse.ArgumentParser()
parser.add_argument('source_root', type=Path)
parser.add_argument('--dry-run', action='store_true')
args = parser.parse_args()
folder = args.source_root / 'source/blender/blenkernel/intern'
target = folder / 'multires_reshape_smooth.cc'
header = folder / 'multires_stable_detail_transport.hh'
original = target.read_bytes()
text = original.decode('utf-8')
newline = '\r\n' if b'\r\n' in original else '\n'
text = text.replace('\r\n', '\n')
if 'SPIKE_DETAIL_ROTATION_V01' in text or 'SPIKE_DETAIL_ROTATION_V02' in text:
    raise SystemExit('Patch gia presente; nessuna modifica.')
compact = 'float3 detail_delta_tangent = float3(0.0f);' in text
full = 'float3x3 tangent_matrix = float3x3::identity();' in text
if compact == full:
    raise SystemExit('Variante cache sconosciuta o ambigua: nessun file modificato.')
anchor = '''        float3 smooth_delta = math::transform_direction(smooth_tangent_matrix,
                                                        original_detail_delta_tangent);'''
replacement = '''        /* SPIKE_DETAIL_ROTATION_V02: opt-in A/B experiment. Native remains default.
         * Preserve residual magnitude without smoothing away intentional peaks. */
        float3 smooth_delta = stable_detail_transport ?
            multires_stable_detail::transport(orig_sculpt_point->tangent_matrix,
                                              smooth_tangent_matrix,
                                              original_detail_delta) :
            math::transform_direction(smooth_tangent_matrix, original_detail_delta_tangent);'''
function_anchor = '''static void evaluate_higher_grid_positions_with_details(
    MultiresReshapeSmoothContext *reshape_smooth_context)
{'''
function_replacement = function_anchor + '''
  const char *transport_mode = std::getenv("BLENDER_MULTIRES_DETAIL_TRANSPORT");
  const bool stable_detail_transport = transport_mode &&
                                      std::strcmp(transport_mode, "rotation") == 0;'''
inverse_anchor = '''        const float3x3 original_sculpt_tangent_matrix_inv = math::invert(
            orig_sculpt_point->tangent_matrix);
        float3 original_detail_delta_tangent = math::transform_direction(
            original_sculpt_tangent_matrix_inv, original_detail_delta);'''
inverse_replacement = '''        float3 original_detail_delta_tangent(0.0f);
        if (!stable_detail_transport) {
          const float3x3 inverse = math::invert(orig_sculpt_point->tangent_matrix);
          original_detail_delta_tangent = math::transform_direction(inverse, original_detail_delta);
        }'''
replacements = [
    ('#include "multires_reshape.hh"', '#include "multires_reshape.hh"\n#include "multires_stable_detail_transport.hh"\n#include <cstring>\n#include <cstdlib>'),
    (function_anchor, function_replacement),
    (inverse_anchor, inverse_replacement),
    (anchor, replacement),
]
if compact:
    replacements = [
        replacements[0],
        ('  const MultiresReshapeContext *reshape_context;','  const MultiresReshapeContext *reshape_context;\n  /* SPIKE_DETAIL_ROTATION_V02: sample mode once for both cache stages. */\n  const bool stable_detail_transport = []() {\n    const char *mode = std::getenv("BLENDER_MULTIRES_DETAIL_TRANSPORT");\n    return mode && std::strcmp(mode, "rotation") == 0;\n  }();'),
        ("  const float3x3 original_sculpt_tangent_matrix_inv = math::invert(tangent_matrix);\n  point->detail_delta_tangent = math::transform_direction(\n      original_sculpt_tangent_matrix_inv, original_detail_delta);",
         "  if (reshape_smooth_context->stable_detail_transport) {\n    point->detail_delta_tangent = multires_stable_detail::encode(\n        tangent_matrix, original_detail_delta);\n  }\n  else {\n    const float3x3 inverse = math::invert(tangent_matrix);\n    point->detail_delta_tangent = math::transform_direction(inverse, original_detail_delta);\n  }"),
        (anchor,
         "        float3 smooth_delta = reshape_smooth_context->stable_detail_transport ?\n            multires_stable_detail::decode(smooth_tangent_matrix, original_detail_delta_tangent) :\n            math::transform_direction(smooth_tangent_matrix, original_detail_delta_tangent);"),
    ]
for old, new in replacements:
    if text.count(old) != 1:
        raise SystemExit('Sorgente non compatibile: ancoraggio mancante/ambiguo. Nessuna modifica.')
    text = text.replace(old, new, 1)
payload = Path(__file__).with_name(header.name).read_bytes()
if header.exists():
    raise SystemExit('Header gia esistente: nessuna modifica.')
if args.dry_run:
    print('DRY RUN PASS: cache ' + ('compatta' if compact else 'completa') + '; nessuna modifica.')
    raise SystemExit(0)
# Current build files stay byte-identical: FaceSets V06d/No-REC, layers,
# bake, Undo and transport brushes. Snapshot only existing paths.
state = json.loads(Path(__file__).with_name('CURRENT-BUILD-PATHS.json').read_text())
protected = {}
for relative in state:
    path = args.source_root / relative
    if path.is_file() and path.resolve() != target.resolve():
        protected[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
backup = args.source_root / ('spike-transport-backup-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
backup.mkdir()
(backup / target.name).write_bytes(original)
patched = text.replace('\n', newline).encode('utf-8')
manifest = {'target': str(target.resolve()), 'sha256': hashlib.sha256(original).hexdigest(),
            'patched_sha256': hashlib.sha256(patched).hexdigest(),
            'header_sha256': hashlib.sha256(payload).hexdigest(),
            'header': str(header.resolve()), 'protected_files': protected,
            'cache_variant': 'compact' if compact else 'full'}
(backup / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
try:
    header.write_bytes(payload)
    target.write_bytes(patched)
    for relative, expected in protected.items():
        if hashlib.sha256((args.source_root / relative).read_bytes()).hexdigest() != expected:
            raise RuntimeError('File della build cambiato durante la patch: ' + relative)
except BaseException:
    target.write_bytes(original)
    header.unlink(missing_ok=True)
    raise
print('V02 installata su cache ' + ('compatta' if compact else 'completa') + '. Backup:', backup)
print('File protetti invariati:', len(protected))
print('Ricompilare Blender. Test: BLENDER_MULTIRES_DETAIL_TRANSPORT=rotation')
