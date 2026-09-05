#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Causal PSVR2 position predictor replay (requires NumPy).

Use --trace-dir /tmp --output /tmp/psvr2-replay. horizon.csv is authoritative;
otherwise pose.csv supplies source/target VTS, never its transformed head pose.
The first half of PID 38153 is development; its second half and other runs are
cross-checks. All were inspected during model selection, so these labels do
not imply an untouched test set. Targets crossing the split are excluded
from development.
"""
import argparse
import csv
import hashlib
import itertools
import json
import subprocess
from pathlib import Path

import numpy as np


def read(path):
    return np.genfromtxt(path, delimiter=",", names=True, dtype=None, encoding="utf-8", ndmin=1)


def xyz(rows, prefix):
    return np.column_stack([rows[prefix + c] for c in "xyz"])


def clip(v, limit):
    return v * np.minimum(1, limit / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-12))


def ema(v, alpha):
    out = v.copy()
    for i in range(1, len(v)):
        out[i] = out[i - 1] + alpha * (v[i] - out[i - 1])
    return out


def states(t, p, v, valid):
    """Each returned state uses samples <= its index; units seconds/metres."""
    z = np.zeros_like(p)
    yield "raw", p, v, z, "cv"
    for alpha in (.1, .25, .5, .75):
        yield f"ema_{alpha}", p, ema(v, alpha), z, "cv"
    dt = np.r_[t[1] - t[0], np.diff(t)]
    # Backward-difference velocities live at interval midpoints.
    a = np.zeros_like(v)
    a[2:] = np.diff(v, axis=0)[1:] / ((dt[2:] + dt[1:-1]) * .5)[:, None]
    for alpha in (.1, .25, .5, 1.):
        smooth = ema(a, alpha)
        for gain in (.25, .5, 1.):
            ac = clip(smooth, 8.) * gain
            yield f"acc_{alpha}_{gain}", p, v + ac * dt[:, None] * .5, ac, "ca"
    for alpha in (.25, .5):
        for limit in (2., 4.):
            for gain in (.25, .5):
                ac = clip(ema(clip(a, limit), alpha), limit) * gain
                ac[np.linalg.norm(v, axis=1) < .01] = 0
                yield f"guard_acc_{alpha}_{gain}_{limit}", p, v + ac * dt[:, None] * .5, ac, "ca"
    for n in (3, 4, 5, 7, 9):
        for degree in (1, 2):
            vel, acc = v.copy(), z.copy()
            for i in range(n - 1, len(t)):
                ts = t[i - n + 1:i + 1] - t[i]
                design = np.column_stack([ts ** k for k in range(degree + 1)])
                fit = np.linalg.lstsq(design, p[i - n + 1:i + 1] - p[i], rcond=None)[0]
                vel[i] = fit[1]
                if degree == 2:
                    acc[i] = 2 * fit[2]
            yield f"ls{degree}_{n}", p, vel, acc, "ca"
            if degree == 2:
                for gain in (.25, .5):
                    ac = clip(acc, 8.) * gain
                    yield f"ls2_{n}_damped_{gain}", p, v + gain * (vel - v), ac, "ca"
    # Frozen driver candidate, including startup/gap resets and stationary gate.
    ac = np.zeros_like(v)
    previous_interval = 0.
    have_velocity = False
    ready = np.zeros(len(t), bool)
    for i in range(len(t)):
        d = t[i]-t[i-1] if i else 0.
        contiguous = valid[i] and have_velocity and .005 <= d <= .035
        ready[i] = contiguous and previous_interval > 0
        if ready[i]:
            observed = clip((v[i]-v[i-1])*2/(d+previous_interval), 2.)
            ac[i] = clip(ac[i-1] + .25*(observed-ac[i-1]), 2.)
        previous_interval = d if contiguous else 0.
        have_velocity = valid[i]
    ac[(np.linalg.norm(v, axis=1) < .01) | ~ready] = 0
    yield "bounded_acceleration", p, v, ac*.5, "bounded"
    for alpha, beta, gamma in ((.5, .1, 0), (.8, .2, 0), (1., .5, 0),
                                (.5, .1, .01), (.8, .3, .05), (1., .5, .1)):
        pos, vel, acc = p.copy(), v.copy(), z.copy()
        for i in range(1, len(t)):
            d = dt[i]
            pp = pos[i-1] + vel[i-1]*d + .5*acc[i-1]*d*d
            vv = vel[i-1] + acc[i-1]*d
            residual = p[i] - pp
            pos[i] = pp + alpha*residual
            vel[i] = vv + beta/d*residual
            acc[i] = clip(acc[i-1] + 2*gamma/(d*d)*residual, 8.)
        yield f"abg_{alpha}_{beta}_{gamma}", pos, vel, acc, "ca"


def load(path):
    s = read(path)
    stem = str(path).removesuffix("_slam.csv")
    hp = Path(stem + "_horizon.csv")
    qp = hp if hp.exists() else Path(stem + "_pose.csv")
    q = read(qp)
    if len(s) < 10 or len(q) < 10:
        return None
    tn = s['slam_vts_ns'].astype(np.int64)
    if not np.all(np.diff(tn) > 0):
        raise ValueError(f"Nonmonotonic SLAM: {path}")
    p, v = xyz(s, 'corrected_pos_'), xyz(s, 'linvel_')
    source = q['source_slam_vts_ns' if hp.exists() else 'latest_slam_vts_ns']
    target = q['target_vts_ns' if hp.exists() else 'requested_vts_ns']
    ix = np.searchsorted(tn, source)
    hi = np.searchsorted(tn, target)
    ix_safe, hi_safe = np.minimum(ix, len(tn)-1), np.clip(hi, 1, len(tn)-1)
    h = (target-source)*1e-9
    good = (ix >= 9) & (ix < len(tn)) & (tn[ix_safe] == source) & (hi > 0) & (hi < len(tn))
    good &= (h > 0) & (h <= .12) & ((tn[hi_safe]-tn[hi_safe-1]) < 35_000_000)
    # Exclude history containing gaps, nonfinite values, invalid position/velocity.
    sample_good = np.isfinite(p).all(axis=1) & np.isfinite(v).all(axis=1) & ((s['relation_flags'] & 6) == 6)
    sample_good[1:] &= np.diff(tn) < 35_000_000
    for lag in range(10):
        good &= sample_good[np.maximum(ix_safe-lag, 0)]
    good &= sample_good[hi_safe] & sample_good[hi_safe-1]
    ix, hi, target, h = ix[good], hi[good], target[good], h[good]
    fraction = (target-tn[hi-1])/(tn[hi]-tn[hi-1])
    actual = p[hi-1] + fraction[:, None]*(p[hi]-p[hi-1])
    segment = p[hi]-p[hi-1]
    direction = segment / np.maximum(np.linalg.norm(segment, axis=1)[:, None], 1e-12)
    direction[np.linalg.norm(segment, axis=1) <= .0001] = 0
    raw = p[ix]+v[ix]*h[:, None]
    checks = {}
    if hp.exists():
        for name, observed, expected in [('actual', actual, xyz(q[good], 'actual_')),
                                        ('raw', raw, xyz(q[good], 'raw_pred_'))]:
            checks[name+'_max_difference_mm'] = float(np.max(np.linalg.norm(observed-expected, axis=1))*1000)
            assert checks[name+'_max_difference_mm'] < .002, checks
    if hp.exists():
        alpha = float(q['filter_alpha'][0])
        assert np.all(q['filter_alpha'] == alpha)
        filtered = p[ix] + ema(v, alpha)[ix]*h[:, None]
        checks['ema_max_difference_mm'] = float(np.max(np.linalg.norm(filtered-xyz(q[good], 'filtered_pred_'), axis=1))*1000)
        assert checks['ema_max_difference_mm'] < .002, checks
    live = None
    if hp.exists():
        poses = read(stem + '_pose.csv')
        pose_map = {int(row['host_query_ns']): row for row in poses}
        assert len(pose_map) == len(poses), 'Ambiguous pose query timestamps'
        matched = np.array([pose_map[int(row['query_host_ns'])] for row in q[good]], dtype=poses.dtype)
        assert np.all(matched['requested_vts_ns'] == target)
        assert np.all(matched['latest_slam_vts_ns'] == tn[ix])
        # Invert the fixed PSVR2 tracker->head translation, using the returned
        # orientation. This is a check on the recorded live path, not a model.
        quat = xyz(matched, 'q')
        offset = np.broadcast_to([.000247, -.000273, .104826], quat.shape)
        rotated = offset + 2*np.cross(quat, np.cross(quat, offset) + matched['qw'][:, None]*offset)
        live = xyz(matched, 'pos_') - rotated
        if stem.endswith('_38153'):
            live_velocity = ema(v, .25)[ix]
            tail_horizon = np.maximum(0, target-np.maximum(tn[ix], matched['latest_imu_vts_ns']))*1e-9
            tail_prediction = p[ix]+live_velocity*tail_horizon[:, None]
            checks['live_gyro_tail_max_difference_mm'] = float(np.max(np.linalg.norm(tail_prediction-live, axis=1))*1000)
            assert checks['live_gyro_tail_max_difference_mm'] < .002, checks
    t = (tn-tn[0])*1e-9
    speed = np.linalg.norm(v[ix], axis=1)
    accel = np.linalg.norm((v[ix]-v[ix-1]) / (t[ix]-t[ix-1])[:, None], axis=1)
    # Retrospective regime label only; never supplied to the predictor.
    reversal = (np.sum(v[ix]*v[hi], axis=1) < 0) & (speed >= .02)
    split = tn[0] + (tn[-1]-tn[0])//2
    phases = {'development': target < split, 'validation': tn[ix] >= split} if stem.endswith('_38153') else {'validation': np.ones(len(ix), bool)}
    masks = {'all': np.ones(len(ix), bool), 'speed_lt_0.01': speed < .01,
             'speed_ge_0.05': speed >= .05, 'speed_ge_0.2': speed >= .2, 'speed_ge_0.4': speed >= .4,
             'accel_lt_0.5': accel < .5, 'accel_ge_2': accel >= 2, 'reversal': reversal,
             'horizon_40_80ms': (h >= .04) & (h <= .08)}
    meta = dict(trace=path.name, samples=len(s), queries=len(q), scored=len(ix), excluded=len(q)-len(ix),
                duration_s=float(t[-1]), horizon_ms=np.percentile(h*1000, [5,50,95]).tolist(), checks=checks,
                sources={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in ([path, qp, Path(stem + '_pose.csv')] if hp.exists() else [path, qp])})
    return t, p, v, ix, h, actual, direction, raw, phases, masks, meta, ((s['relation_flags'] & 6) == 6) & np.isfinite(p).all(axis=1) & np.isfinite(v).all(axis=1), live


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace-dir', type=Path, default=Path('/tmp'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pids', nargs='*')
    parser.add_argument('--c-predictor', type=Path, help='Compiled tests_psvr2_linear_prediction executable for parity checks')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records, metadata = [], []
    for path in sorted(args.trace_dir.glob('monado_psvr2_*_slam.csv')):
        pid = path.name.split('_')[2]
        if args.pids and pid not in args.pids:
            continue
        data = load(path)
        if data is None:
            continue
        t,p,v,ix,h,actual,direction,raw,phases,masks,meta,valid,live = data
        metadata.append(meta)
        baseline = np.linalg.norm(actual-raw, axis=1)*1000
        candidates = states(t,p,v,valid)
        if live is not None:
            candidates = itertools.chain(candidates, [('recorded_live_tracker', p, v, np.zeros_like(p), 'recorded')])
        for name, pos, vel, acc, kind in candidates:
            prediction = pos[ix]+vel[ix]*h[:,None]+.5*acc[ix]*h[:,None]**2
            if kind == "recorded":
                prediction = live
            if kind == "bounded":
                hc = np.minimum(h, .08)
                interval = t[ix]-t[ix-1]
                prediction = p[ix]+v[ix]*h[:,None]+.5*acc[ix]*(hc*(hc+interval))[:,None]
                if args.c_predictor:
                    order = np.argsort(ix, kind='stable')
                    commands, qi = [], 0
                    for i in range(len(t)):
                        ns = int(round(t[i]*1e9))
                        values = ' '.join(format(x, '.12g') for x in [*p[i], *v[i]])
                        commands.append(f'S {ns} {values} {int(valid[i])}')
                        while qi < len(order) and ix[order[qi]] == i:
                            commands.append(f'P {ns+int(round(h[order[qi]]*1e9))}')
                            qi += 1
                    result = subprocess.run([str(args.c_predictor), '--replay'], input='\n'.join(commands)+'\n',
                                            text=True, capture_output=True, check=True)
                    cp = np.array([[float(x) for x in line.split()] for line in result.stdout.splitlines()])
                    delta = np.max(np.linalg.norm(cp-prediction[order], axis=1))*1000
                    meta['c_parity_max_difference_mm'] = float(delta)
                    assert delta < .002, (path, delta)
            error = np.linalg.norm(actual-prediction, axis=1)*1000
            along = np.sum((actual-prediction)*direction, axis=1)*1000
            for phase, phase_mask in dict(full=np.ones(len(ix),bool), **phases).items():
                for regime, mask in masks.items():
                    m = phase_mask & mask
                    if not m.any():
                        continue
                    pct = np.percentile(error[m], [50,95,99,100])
                    records.append(dict(pid=pid,phase=phase,regime=regime,model=name,n=int(m.sum()),
                                        median_mm=pct[0],p95_mm=pct[1],p99_mm=pct[2],max_mm=pct[3],
                                        along_median_mm=float(np.median(along[m])),along_mean_mm=float(np.mean(along[m])),
                                        win_pct=float(np.mean(error[m] < baseline[m])*100)))
        print(pid,meta['scored'],meta['horizon_ms'],flush=True)
    if not records:
        parser.error('No eligible recordings with at least ten samples and queries')
    with (args.output/'summary.csv').open('w') as f:
        writer=csv.DictWriter(f,fieldnames=list(records[0]));writer.writeheader();writer.writerows(records)
    (args.output/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')


if __name__ == '__main__':
    main()
