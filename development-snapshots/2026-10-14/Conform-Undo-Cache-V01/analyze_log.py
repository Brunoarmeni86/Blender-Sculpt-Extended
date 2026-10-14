"""Report data changes without treating a checksum as proof of their cause."""
import argparse
import json
from pathlib import Path

def records(text):
    items = []
    layers = []
    for line in text.splitlines():
        if line.startswith('[MULTIRES_AUDIT_BEGIN] '):
            layers = []
        elif line.startswith('[MULTIRES_LAYER_AUDIT] '):
            layers.append(json.loads(line.split('] ', 1)[1]))
        elif line.startswith('[MULTIRES_BASE_AUDIT] '):
            item = json.loads(line.split('] ', 1)[1])
            item['layer_payloads'] = list(layers)
            items.append(item)
    return items

def compare(items):
    results = []
    for a, b in zip(items, items[1:]):
        comparable = all(a.get(k) == b.get(k) for k in ('object', 'modifier', 'top', 'verts', 'corners'))
        valid = a['invalid'] == b['invalid'] == 0 and a['native_available'] and b['native_available']
        results.append({
            'from': a['label'], 'to': b['label'], 'same_layout': comparable,
            'valid_native_buffers': bool(valid),
            'native_base_changed': a['native_hash'] != b['native_hash'],
            'cage_changed': a['cage_hash'] != b['cage_hash'],
            'layer_metadata_changed': a['metadata_hash'] != b['metadata_hash'],
            'layer_payloads_changed': a['layer_payloads'] != b['layer_payloads'],
            'surface_changed': a['surface_hash'] != b['surface_hash'],
            'pending_before': a['pending_coords'], 'pending_after': b['pending_coords'],
        })
    return results

def main():
    p = argparse.ArgumentParser()
    p.add_argument('log', type=Path)
    a = p.parse_args()
    items = records(a.log.read_text(encoding='utf-8', errors='replace'))
    report = {'status': 'OBSERVATIONS_ONLY', 'snapshots': items, 'transitions': compare(items),
              'limitations': 'Hashes identify buffer changes, not their cause. Legitimate base edits and changes of topology must be excluded using the operation sequence.'}
    output = a.log.with_suffix('.audit.json')
    output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    print(output)
    for item in report['transitions']:
        print(item)
    if not items:
        raise RuntimeError('Nessun fingerprint nel log: usare i pulsanti Diagnosi Undo / Base nella build compilata dal kit.')

if __name__ == '__main__':
    main()
