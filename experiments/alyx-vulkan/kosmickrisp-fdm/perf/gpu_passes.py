#!/usr/bin/env python3
"""Sum GPU time per encoder label for one process in a Metal System Trace.

usage: gpu_passes.py TRACE PROCESS_SUBSTRING
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from xctrace_table import rows

trace, proc = sys.argv[1], sys.argv[2]
per = collections.defaultdict(lambda: collections.Counter())
count = collections.Counter()
t_min, t_max = None, None
for r in rows(trace, 'metal-gpu-intervals'):
    if proc not in r.get('process', ('', ''))[0]:
        continue
    label = r['event-label'][0]
    label = re.sub(r'^Command Buffer \d+:', '', label)
    label = re.sub(r'\s+\(.*\)\s+0x[0-9a-f]+$', '', label).strip()
    ch = r['channel-name'][0]
    d = int(r['duration'][1])
    s = int(r['start'][1])
    t_min = s if t_min is None else min(t_min, s)
    t_max = s + d if t_max is None else max(t_max, s + d)
    per[label][ch] += d
    count[(label, ch)] += 1
span = (t_max - t_min) / 1e9
print(f'span {span:.1f} s')
total = collections.Counter()
for label, chans in per.items():
    for ch, d in chans.items():
        total[ch] += d
print('channel totals (ms per s of trace):',
      {ch: round(d / 1e6 / span, 1) for ch, d in total.items()})
items = sorted(per.items(), key=lambda kv: -sum(kv[1].values()))
print(f'{"ms/s":>7} {"frag":>7} {"vert":>7} {"comp":>7} {"n/s":>6}  label')
for label, chans in items[:40]:
    tot = sum(chans.values()) / 1e6 / span
    n = max(count[(label, c)] for c in chans) / span
    print(f'{tot:7.1f} {chans["Fragment"]/1e6/span:7.1f} {chans["Vertex"]/1e6/span:7.1f} '
          f'{chans["Compute"]/1e6/span:7.1f} {n:6.1f}  {label[:110]}')
