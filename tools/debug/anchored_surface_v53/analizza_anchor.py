"""Summarize optional Blender Anchor diagnostics; standard library only."""
import argparse
import collections
import json
import math
from pathlib import Path
import statistics

MODES = ('planar', 'v1', 'v2', 'v3', 'v4', 'v5', 'rejected')


def read_rows(path):
    # Windows PowerShell Tee-Object writes UTF-16; redirected logs may be UTF-8.
    data = Path(path).read_bytes()
    text = data.decode('utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig', errors='replace')
    rows, malformed = [], 0
    for line in text.splitlines():
        if '[ANCHOR_DIAG]' not in line:
            continue
        fields = dict(item.split('=', 1) for item in line.split('[ANCHOR_DIAG]', 1)[1].split() if '=' in item)
        try:
            row = dict(fields)
            for name in MODES + ('requested', 'built'):
                row[name] = int(fields[name])
            for name in ('radius', 'prepare_ms', 'search_ms', 'relax_ms', 'audit_ms'):
                row[name] = float(fields[name])
                if not math.isfinite(row[name]):
                    raise ValueError(name)
            parts = fields['pass'].split('/')
            if len(parts) != 3:
                raise ValueError('pass')
            row['pass_ids'] = tuple(int(value) for value in parts)
            rows.append(row)
        except (KeyError, ValueError):
            malformed += 1
    return rows, malformed


def timings(rows, name):
    values = sorted(row[name] for row in rows)
    return {'count': len(values), 'sum_ms': sum(values),
            'median_ms': statistics.median(values) if values else 0,
            'p95_ms': values[max(0, math.ceil(.95 * len(values)) - 1)] if values else 0,
            'max_ms': max(values, default=0)}


def summarize(rows, malformed):
    totals = {name: sum(row[name] for row in rows) for name in MODES}
    accepted = sum(totals[name] for name in MODES if name != 'rejected')
    passes = {}
    for key in sorted({row['pass'] for row in rows}):
        group = [row for row in rows if row['pass'] == key]
        eligible = [row for row in group if row['requested']]
        passes[key] = {'reports': len(group), 'requested_reports': len(eligible),
                       'transport_ready_reports': sum(row['built'] for row in eligible),
                       'texture_calls': {name: sum(row[name] for row in group) for name in MODES},
                       'prepare': timings(group, 'prepare_ms')}
    requested = [row for row in rows if row['requested']]
    seam_ratios = []
    for row in requested:
        if row['radius'] > 0:
            try:
                value = float(row.get('seam_uv_max', 0)) / row['radius']
                if math.isfinite(value):
                    seam_ratios.append(value)
            except ValueError:
                pass
    return {'reports': len(rows), 'malformed_reports': malformed,
            'requested_reports': len(requested), 'texture_calls': totals,
            'mode_percent_of_accepted_calls': {name: 100 * totals[name] / accepted if accepted else 0
                                             for name in MODES if name != 'rejected'},
            'reasons': dict(collections.Counter(row.get('reason', 'unknown') for row in requested)),
            'cache_events': dict(collections.Counter(row.get('cache', 'unknown') for row in requested)),
            'timings': {name: timings(requested, name) for name in
                        ('prepare_ms', 'search_ms', 'relax_ms', 'audit_ms')},
            'passes': passes,
            'max_reported_seam_uv_over_radius': max(seam_ratios, default=0),
            'notes': ['Counts are texture calls, not unique vertices or strokes.',
                      'prepare_ms includes search, relaxation and audit; do not sum these timings.',
                      'Diagnostic atomics and level 2 audits add overhead; these are not normal-mode benchmarks.',
                      'Transport readiness does not imply that every texture call used its UV.',
                      'Pass IDs identify mirror/radial/tile passes; absence of a pass is not proof of support failure.',
                      'Reported seam checks are partial diagnostics, not proof of global continuity.']}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    rows, malformed = read_rows(args.log)
    if not rows:
        raise SystemExit('Nessun report Anchor valido. Avvia con diagnostica e prova una pennellata Anchored.')
    result = summarize(rows, malformed)
    output = args.output or args.log.with_suffix('.summary.json')
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    print('Report:', len(rows), '| report richiesti:', result['requested_reports'])
    for name, percent in result['mode_percent_of_accepted_calls'].items():
        print(f'  {name}: {percent:.2f}% delle chiamate texture accettate')
    timing = result['timings']['prepare_ms']
    print(f"Preparazione: mediana {timing['median_ms']:.3f} ms, p95 {timing['p95_ms']:.3f} ms")
    print('Passate osservate:', ', '.join(result['passes']))
    print('Riepilogo:', output)


if __name__ == '__main__':
    main()
