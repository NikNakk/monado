#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Replay fixed recordings with three absolute LED models, without modifying the datasets.

Run from the repository root. Raw outputs go under ignored doc/results/.
The Toolkit table is pinned beside this script; see TOOLKIT-LICENSE.txt.
"""
import argparse
import concurrent.futures
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
EXTERNAL = Path('/Volumes/nick-mac-external/psvr2-datasets/sessions')
SESSIONS = [
    EXTERNAL / name / 'constellation.ctd' for name in (
        '20261003-234218-corr-off-grip',
        '20261003-234436-corr-on-grip',
        '20261003-235855-corr-on-grip-2',
        '20261004-000108-hard-cases',
        '20261004-000405-hard-cases',
        '20261004-000909-distance',
        '20261005-2001-openbrush-oriented-recorded',
    )
] + [ROOT / 'build/arm64/controller-evidence/20261025-controller-survey-both-mode16-v3/constellation.ctd']
MODELS = ('sony', 'local', 'toolkit')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs)-1, int(p*(len(xs)-1)+0.5))]


def model_tables():
    header = (ROOT / 'src/xrt/drivers/pssense/pssense_led_model.h').read_text()
    correction = (ROOT / 'src/xrt/drivers/pssense/pssense_led_correction.h').read_text()
    sony, local = {}, {}
    for dev, side in enumerate(('left', 'right')):
        section = header.split('pssense_' + side + '_leds[]')[1].split('};')[0]
        ps = [[float(x) for x in row.split(',')] for row in re.findall(r'\.position = \{([^}]+)\}', section)]
        section = correction.split('pssense_' + side + '_led_corrections[]')[1].split('};')[0]
        ds = [[float(x.strip().rstrip('f')) for x in row.split(',')] for row in re.findall(r'\{([^{}]+)\}', section)]
        assert len(ps) == len(ds) == 17
        # Source header is XR; datasets / --led-offsets use CV (negate y,z).
        sony[dev] = [[x, -y, -z] for x, y, z in ps]
        local[dev] = [[p[0]+d[0], -p[1]-d[1], -p[2]-d[2]] for p, d in zip(ps, ds)]
    table = json.loads((HERE / 'toolkit-right-mm.json').read_text())['positions_mm']
    table = table[1:] + table[:1]  # Toolkit LED16 first -> Monado LED0 first.
    assert len(table) == 17
    toolkit = {}
    for dev in range(2):
        toolkit[dev] = [[(x if dev == 1 else -x)/1000, -y/1000, -z/1000] for x,y,z in table]
    return dict(sony=sony, local=local, toolkit=toolkit)


def read_poses(path):
    with path.open() as f:
        return [{k: int(v) if k in ('timestamp_ns','device','cameras','matches') else float(v)
                 for k,v in r.items()} for r in csv.DictReader(f)]


def score(rows, exposures, start, end):
    ts = [r['timestamp_ns'] for r in rows]
    gaps = [(b-a)/1e9 for a,b in zip(ts,ts[1:])]
    # No accepted solve; leading/trailing intervals are included in longest silence.
    silence = gaps + ([(ts[0]-start)/1e9,(end-ts[-1])/1e9] if ts else [(end-start)/1e9])
    return dict(poses=len(rows), solved_pct=100*len(rows)/exposures,
                rms_p50=pct([r['rms_px'] for r in rows],.5), rms_p95=pct([r['rms_px'] for r in rows],.95),
                matches_p50=pct([r['matches'] for r in rows],.5),
                gaps_over_100ms=sum(g > .1 for g in gaps), longest_silence_s=max(silence))


def main():
    ap=argparse.ArgumentParser(__doc__)
    ap.add_argument('--out', type=Path, default=ROOT/'doc/results/pssense-led-model-20261117')
    ap.add_argument('--jobs', type=int, default=2)
    args=ap.parse_args()
    out=args.out.resolve(); out.mkdir(parents=True,exist_ok=True)
    exe=ROOT/'build/arm64/src/xrt/tracking/constellation/constellation_replay'
    env={k:v for k,v in os.environ.items() if not k.startswith(('CONSTELLATION_', 'PSSENSE_', 'PSVR2_'))}
    settings={'CONSTELLATION_TRACKER_ORIENTED_BOOTSTRAP':'1', 'CONSTELLATION_TRACKER_PRIOR_REACQUIRE':'0',
              'CONSTELLATION_TRACKER_RERUN_ENABLE':'0'}
    env.update(settings)
    targets=model_tables()
    metadata=dict(source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                  replay=str(exe), replay_sha256=sha(exe),environment=settings,sessions=[])
    tasks=[]
    for path in SESSIONS:
        name=path.parent.name; dest=out/name; dest.mkdir(exist_ok=True)
        geom=dest/'recorded'
        cmd=[str(exe),str(path),'--geometry',str(geom)]
        r=subprocess.run(cmd,env=env,capture_output=True,text=True,check=True)
        (dest/'inventory.log').write_text(r.stdout+r.stderr)
        assert 'clean end of file' in r.stderr+r.stdout
        with (dest/'recorded-leds.csv').open() as f: leds=list(csv.DictReader(f))
        assert len(leds)==34 and {int(r['device']) for r in leds}=={0,1}
        with (dest/'recorded-cameras.csv').open() as f:
            sample_times=sorted(int(r['timestamp_ns']) for r in csv.DictReader(f))
        times=[]
        for t in sample_times:
            if not times or t-times[-1]>2_000_000:
                times.append(t)
        exp=re.search(r'exposures: (\d+) over ([\d.]+) s',r.stdout)
        assert len(times)==int(exp[1])
        info=dict(name=name,path=str(path),sha256=sha(path),exposures=len(times),seconds=float(exp[2]),
                  start=times[0],end=times[-1], annotations=re.findall(r'annotation at (\d+) device (\d+): (\S+)',r.stdout))
        metadata['sessions'].append(info)
        # Export uses six decimal places (0.5 um rounding); generate deltas from that exact export.
        for model in MODELS:
            offsets=dest/(model+'-offsets.csv')
            with offsets.open('w') as f:
                w=csv.writer(f); w.writerow(['device','led','dx_mm','dy_mm','dz_mm'])
                for row in leds:
                    dev, led=int(row['device']),int(row['led'])
                    delta=[1000*(target-float(row[axis])) for target,axis in zip(targets[model][dev][led],('px','py','pz'))]
                    w.writerow([dev,led]+[f'{v:.9f}' for v in delta])
            tasks.append((info,model,dest,offsets))
    (out/'manifest.json').write_text(json.dumps(metadata,indent=2))

    def run(task):
        info, model, dest, offsets=task
        poses=dest/(model+'-poses.csv'); log=dest/(model+'.log')
        cmd=[str(exe),info['path'],'--tracker-filter','--tracker-csv',str(poses),'--led-offsets',str(offsets)]
        receipt=dest/(model+'-receipt.json')
        signature=dict(command=cmd, environment=settings, replay_sha256=metadata['replay_sha256'],
                       dataset_sha256=info['sha256'], offsets_sha256=sha(offsets))
        if not (receipt.exists() and json.loads(receipt.read_text())==signature):
            started=time.monotonic()
            with log.open('w') as f:
                r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT)
            if r.returncode:
                raise RuntimeError(f'{info["name"]} {model}: exit {r.returncode}; see {log}')
            receipt.write_text(json.dumps(signature,indent=2))
            print(f'{info["name"]} {model}: {time.monotonic()-started:.1f}s',flush=True)
        text=log.read_text()
        assert 'applied to 34 LEDs' in text and 'tracker replay (joint path)' in text
        rows=read_poses(poses)
        result=[]
        for dev in range(2):
            subset=[r for r in rows if r['device']==dev]
            result.append(dict(session=info['name'],model=model,device=dev,
                               **score(subset,info['exposures'],info['start'],info['end'])))
        return result
    results=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(run,tasks): results.extend(result)
    # Shared timestamp RMS avoids comparing easy solves in one run with harder solves in another.
    for info in metadata['sessions']:
        dest=out/info['name']
        poses={m:read_poses(dest/(m+'-poses.csv')) for m in MODELS}
        for dev in range(2):
            maps={m:{r['timestamp_ns']:r for r in poses[m] if r['device']==dev} for m in MODELS}
            shared=set.intersection(*(set(v) for v in maps.values()))
            for m in MODELS:
                result=next(r for r in results if (r['session'],r['model'],r['device'])==(info['name'],m,dev))
                result.update(shared_poses=len(shared),shared_rms_p50=pct([maps[m][t]['rms_px'] for t in shared],.5),
                              shared_rms_p95=pct([maps[m][t]['rms_px'] for t in shared],.95))
    (out/'summary.json').write_text(json.dumps(results,indent=2))
    with (out/'summary.csv').open('w') as f:
        w=csv.DictWriter(f,fieldnames=list(results[0]));w.writeheader();w.writerows(results)
    print('DONE',out,flush=True)

if __name__=='__main__': main()
