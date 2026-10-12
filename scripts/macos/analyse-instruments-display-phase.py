#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Relate late or missing PS VR2 frames to other displays' refresh phase in an Instruments trace.

Exports the Display tables of a `.trace` recorded with the Display instrument,
finds the display that shows monado-service surfaces, and reports (a) per
second, how many Monado swaps reached the display and how many were late, and
(b) the missing-or-late rate binned by the HMD refresh's phase after the most
recent refresh of every other display. Results stay outside Git.
"""
import argparse
import bisect
import collections
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def export(trace, schema, out):
    path = out/f'{schema}.xml'
    if not path.exists():
        with path.open('w') as f:
            subprocess.run(['xcrun', 'xctrace', 'export', '--input', str(trace), '--xpath',
                            f'/trace-toc/run[@number="1"]/data/table[@schema="{schema}"]'],
                           stdout=f, check=True)
    return path


def rows(path):
    """Yield rows as {mnemonic: (fmt, text)}, resolving xctrace id/ref sharing."""
    ids, columns = {}, []
    for _, el in ET.iterparse(path, events=('end',)):
        if el.tag == 'mnemonic':
            columns.append(el.text)
        elif el.tag == 'row':
            values = []
            for child in el:
                ref = child.get('ref')
                values.append(ids.get(ref, ('', '')) if ref else (child.get('fmt', ''), child.text or ''))
            yield dict(zip(columns, values))
            el.clear()
        if el.get('id') is not None:
            ids[el.get('id')] = (el.get('fmt', ''), el.text or '')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='Directory for exports and results')
    parser.add_argument('--late-ms', type=float, default=5.0, help='Swap later than desired by this counts as late')
    parser.add_argument('--start', type=float, default=0.0, help='Ignore refreshes before this trace time (s)')
    parser.add_argument('--end', type=float, default=1e9, help='Ignore refreshes after this trace time (s)')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    # Monado's surfaces, and the display they are shown on.
    surfaces, hmd = set(), collections.Counter()
    for row in rows(export(args.trace, 'displayed-surfaces-interval', args.output)):
        if 'monado-service' in row['event-label'][0]:
            surfaces.add(int(row['surface-id'][0]))
            hmd[row['display-name'][0]] += 1
    if not surfaces:
        raise SystemExit('no monado-service surfaces in displayed-surfaces-interval')
    hmd_name = hmd.most_common(1)[0][0]

    vsyncs = collections.defaultdict(list)
    for row in rows(export(args.trace, 'display-vsyncs-interval', args.output)):
        vsyncs[row['display-name'][0]].append(int(row['timestamp'][1]))
    for times in vsyncs.values():
        times.sort()
    hmd_vsyncs = vsyncs.pop(hmd_name)
    period = sorted(b-a for a, b in zip(hmd_vsyncs, hmd_vsyncs[1:]))[len(hmd_vsyncs)//2]

    swaps = []
    for row in rows(export(args.trace, 'display-surface-swap', args.output)):
        if int(row['surface-id'][0], 16) in surfaces and row['desired-presentation-time'][1]:
            swaps.append((int(row['timestamp'][1]), int(row['desired-presentation-time'][1])))
    swaps.sort()
    swap_times = [s for s, _ in swaps]
    late = {s for s, d in swaps if s-d > args.late_ms*1e6}

    per_second = collections.defaultdict(lambda: {'swaps': 0, 'late': 0})
    for s, _ in swaps:
        per_second[int(s/1e9)]['swaps'] += 1
        per_second[int(s/1e9)]['late'] += s in late

    result = {'trace': str(args.trace), 'hmd_display': hmd_name, 'hmd_period_ms': period/1e6,
              'monado_surfaces': sorted(surfaces), 'per_second': dict(sorted(per_second.items())),
              'phase': {}}
    first, last = swap_times[0], swap_times[-1]
    for name, other in vsyncs.items():
        if len(other) < 2:
            continue
        rate = 1e9*(len(other)-1)/(other[-1]-other[0])
        bins = collections.defaultdict(lambda: [0, 0])
        for t in hmd_vsyncs:
            i = bisect.bisect_right(other, t)-1
            if i < 0 or not first <= t <= last or not args.start <= t/1e9 <= args.end:
                continue
            # Count only while Monado is presenting: a swap within 50 ms on both sides.
            k = bisect.bisect_left(swap_times, t)
            if k == 0 or k == len(swap_times) or t-swap_times[k-1] > 50e6 or swap_times[k]-t > 50e6:
                continue
            phase = ((t-other[i]) % period)/1e6
            j = bisect.bisect_left(swap_times, t-1e6)
            hits = [x for x in swap_times[j:j+2] if abs(x-t) < 2e6]
            bucket = bins[round(int(phase*2)/2, 1)]
            bucket[0] += 1
            bucket[1] += (not hits) or any(x in late for x in hits)
        result['phase'][name] = {'refresh_hz': rate, 'bins_ms': {k: {'vsyncs': v[0], 'missing_or_late': v[1]}
                                                                 for k, v in sorted(bins.items())}}
    (args.output/'display-phase.json').write_text(json.dumps(result, indent=2)+'\n')

    print(f'HMD display {hmd_name}: period {period/1e6:.4f} ms, Monado surfaces {sorted(surfaces)}')
    for name, data in result['phase'].items():
        print(f'\nHMD refresh phase after {name} refresh ({data["refresh_hz"]:.3f} Hz):')
        for k, v in data['bins_ms'].items():
            print(f'  {k:4.1f} ms  {v["missing_or_late"]:4d}/{v["vsyncs"]:4d}  '
                  f'{100*v["missing_or_late"]/max(v["vsyncs"], 1):5.1f}%')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
