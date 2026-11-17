#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Verify geometry/repeatability and measure spread in self-reported static intervals.

Some markers include movement. These spreads are diagnostics, not a validated noise floor.
"""
import csv
import json
import math
import os
from pathlib import Path
import re
import subprocess
import run

out=run.ROOT/'doc/results/pssense-led-model-20261117'
manifest=json.loads((out/'manifest.json').read_text())
summary=json.loads((out/'summary.json').read_text())
targets=run.model_tables()
exe=manifest['replay']
env={k:v for k,v in os.environ.items() if not k.startswith(('CONSTELLATION_','PSSENSE_','PSVR2_'))}
env.update(manifest['environment'])
validation={'geometry':[], 'repeat':[], 'marked_interval_spread':[], 'common_pose_difference':[]}
for info in manifest['sessions']:
    dest=out/info['name']
    for model in run.MODELS:
        prefix=dest/(model+'-verified')
        cmd=[exe,info['path'],'--led-offsets',str(dest/(model+'-offsets.csv')),'--geometry',str(prefix)]
        r=subprocess.run(cmd,env=env,capture_output=True,text=True,check=True)
        assert 'applied to 34 LEDs' in r.stdout
        with Path(str(prefix)+'-leds.csv').open() as f: leds=list(csv.DictReader(f))
        err=max(abs(float(row[axis])-targets[model][int(row['device'])][int(row['led'])][k])
                for row in leds for k,axis in enumerate(('px','py','pz')))
        assert err <= 1.01e-6, err
        validation['geometry'].append(dict(session=info['name'],model=model,max_error_um=err*1e6))
        # Large camera exports are already retained once per recording.
        Path(str(prefix)+'-cameras.csv').unlink()
    poses={m:run.read_poses(dest/(m+'-poses.csv')) for m in run.MODELS}
    for dev in range(2):
        maps={m:{r['timestamp_ns']:r for r in poses[m] if r['device']==dev} for m in run.MODELS}
        common=set(maps['local']) & set(maps['toolkit'])
        delta=[1000*math.sqrt(sum((maps['local'][t][a]-maps['toolkit'][t][a])**2 for a in ('px','py','pz'))) for t in common]
        validation['common_pose_difference'].append(dict(session=info['name'],device=dev,n=len(delta),
                                                    position_mm_p50=run.pct(delta,.5),position_mm_p95=run.pct(delta,.95)))
    # Score only explicitly Create-marked static intervals. Markers are self-reported; no ground truth.
    begins={}
    for timestamp,dev,kind in info['annotations']:
        dev=int(dev);timestamp=int(timestamp)
        if kind=='static_begin': begins[dev]=timestamp
        elif kind=='static_end' and dev in begins:
            start=begins.pop(dev)
            for model in run.MODELS:
                rows=[r for r in poses[model] if r['device']==dev and start<=r['timestamp_ns']<timestamp]
                windows={}
                for row in rows: windows.setdefault((row['timestamp_ns']-start)//1_000_000_000,[]).append(row)
                jitter=[]
                for block in windows.values():
                    if len(block)<15 or any(b['timestamp_ns']-a['timestamp_ns']>100_000_000 for a,b in zip(block,block[1:])): continue
                    mean=[sum(r[a] for r in block)/len(block) for a in ('px','py','pz')]
                    jitter.append(1000*math.sqrt(sum(sum((r[a]-m)**2 for a,m in zip(('px','py','pz'),mean)) for r in block)/len(block)))
                validation['marked_interval_spread'].append(dict(session=info['name'],device=dev,start_ns=start,end_ns=timestamp,
                    model=model,poses=len(rows),windows=len(jitter),spread_mm_p50=run.pct(jitter,.5),spread_mm_p95=run.pct(jitter,.95)))
# Repeat the first recording with both corrected models. Compare timestamped poses byte-for-byte.
info=manifest['sessions'][0]; dest=out/info['name']
for model in ('local','toolkit'):
    path=dest/(model+'-repeat-poses.csv')
    r=subprocess.run([exe,info['path'],'--tracker-filter','--tracker-csv',str(path),'--led-offsets',str(dest/(model+'-offsets.csv'))],env=env,capture_output=True,text=True,check=True)
    same=path.read_bytes()==(dest/(model+'-poses.csv')).read_bytes()
    validation['repeat'].append(dict(session=info['name'],model=model,identical_poses=same))
    assert same, 'deterministic replay changed'
(out/'validation.json').write_text(json.dumps(validation,indent=2))
print('Geometry verified:',len(validation['geometry']),'models, max error um',max(r['max_error_um'] for r in validation['geometry']))
print('Repeat:',validation['repeat'])
for r in validation['marked_interval_spread']: print(r)
