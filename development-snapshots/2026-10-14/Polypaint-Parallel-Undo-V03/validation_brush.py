"""Factory-test brush only. Local asset selection is asynchronous in Blender's UI.
Use the supported local asset operator; Paint.brush itself is read-only.
"""
import os
import time
import bpy

_name = None
_deadline = None
_attempts = 0


def create():
    global _name, _deadline, _attempts
    brush = bpy.data.brushes.new(name='Polypaint Validation Paint', mode='SCULPT')
    brush.use_paint_sculpt = True
    brush.sculpt_brush_type = 'PAINT'
    for prop, value in (('flow', 1.0), ('density', 1.0), ('wet_mix', 0.0),
                        ('wet_persistence', 0.0)):
        if hasattr(brush, prop):
            setattr(brush, prop, value)
    brush.asset_mark()
    if brush.asset_data is None:
        raise RuntimeError('Impossibile marcare il pennello locale di test come asset')
    _name = brush.name
    _deadline = time.monotonic() + 60.0
    _attempts = 0
    return brush


def activate(report):
    global _attempts
    if not _name or _deadline is None:
        raise RuntimeError('Pennello locale di test non preparato')
    obj = bpy.context.object
    if obj is None or obj.mode != 'SCULPT':
        raise RuntimeError('Preparazione Paint richiede Sculpt Mode: '+str(getattr(obj, 'mode', None)))
    _attempts += 1
    status = report.setdefault('brush_setup', {'source':'LOCAL', 'name':_name})
    status['attempts'] = _attempts
    try:
        # AssetRepresentation::library_relative_identifier uses the native path separator.
        result = bpy.ops.brush.asset_activate(
            asset_library_type='LOCAL', asset_library_identifier='',
            relative_asset_identifier=os.path.join('Brush', _name))
        status['operator_result'] = sorted(result)
    except RuntimeError as error:
        status['last_error'] = str(error)
    paint = bpy.context.scene.tool_settings.sculpt
    active = paint.brush if paint else None
    status['active_name'] = active.name if active else None
    status['active_type'] = active.sculpt_brush_type if active else None
    if active and active.name == _name and active.sculpt_brush_type == 'PAINT':
        status['status'] = 'PASS'
        print('[VALIDATION_BRUSH] source=LOCAL name='+active.name+' attempts='+str(_attempts), flush=True)
        return True
    status['status'] = 'WAITING'
    if time.monotonic() >= _deadline:
        raise RuntimeError('Attivazione pennello locale non completata: '+str(status))
    if _attempts == 1 or _attempts % 10 == 0:
        print('[VALIDATION_BRUSH] waiting '+str(status), flush=True)
    return False
