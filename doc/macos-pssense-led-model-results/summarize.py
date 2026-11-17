#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Pool accepted solves and common-timestamp errors; emit inspectable tables."""
import csv
import json
from pathlib import Path
import run

out=run.ROOT/'doc/results/pssense-led-model-20261117'
manifest=json.loads((out/'manifest.json').read_text())
pooled=[]
for group,infos in [('October 3-4',manifest['sessions'][:6]),('OpenBrush',manifest['sessions'][6:7]),('Survey',manifest['sessions'][7:])]:
    for dev in range(2):
        gathered={m:[] for m in run.MODELS}; shared={m:[] for m in run.MODELS}
        for info in infos:
            maps={}
            for m in run.MODELS:
                rows=[r for r in run.read_poses(out/info['name']/(m+'-poses.csv')) if r['device']==dev]
                maps[m]={r['timestamp_ns']:r for r in rows}
                assert len(rows)==len(maps[m]), 'multiple pushes in one exposure'
            common=set.intersection(*(set(v) for v in maps.values()))
            for m in run.MODELS:
                gathered[m].extend(maps[m].values());shared[m].extend(maps[m][t] for t in common)
        n=sum(s['exposures'] for s in infos)
        for m in run.MODELS:
            pooled.append(dict(group=group,device=dev,model=m,exposures=n,poses=len(gathered[m]),
                solved_pct=100*len(gathered[m])/n,shared_poses=len(shared[m]),
                shared_rms_p50=run.pct([r['rms_px'] for r in shared[m]],.5),
                shared_rms_p95=run.pct([r['rms_px'] for r in shared[m]],.95)))
with (out/'pooled.csv').open('w') as f:
    w=csv.DictWriter(f,fieldnames=list(pooled[0]));w.writeheader();w.writerows(pooled)
print('| Recording group | Hand | Sony solve % | Local solve % | Toolkit solve % | Sony / local / Toolkit common error, px |')
print('| --- | --- | ---: | ---: | ---: | --- |')
for group in dict.fromkeys(r['group'] for r in pooled):
    for dev in range(2):
        row=[next(r for r in pooled if (r['group'],r['device'],r['model'])==(group,dev,m)) for m in run.MODELS]
        print('| '+group+' | '+('L' if dev==0 else 'R')+' | '+' | '.join(f'{r["solved_pct"]:.2f}' for r in row)+' | '+' / '.join(f'{r["shared_rms_p50"]:.3f}' for r in row)+' |')
