#!/usr/bin/env python3
import argparse
import csv
import math
import statistics


def values(rows, key):
    out = []
    for r in rows:
        try:
            v = float(r[key])
        except (ValueError, KeyError):
            continue
        if math.isfinite(v):
            out.append(v)
    return out


def q(xs, p):
    if not xs:
        return float('nan')
    xs = sorted(xs)
    pos = (len(xs) - 1) * p
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return xs[lo]
    return xs[lo] * (hi - pos) + xs[hi] * (pos - lo)


def fmt(x):
    return "n/a" if not math.isfinite(x) else f"{x:.3f}"


def summarize(rows, key, label):
    xs = values(rows, key)
    if not xs:
        return
    print(f"{label:30s} n={len(xs):6d}  median={fmt(statistics.median(xs)):>8s} ms  p95={fmt(q(xs,.95)):>8s}  p99={fmt(q(xs,.99)):>8s}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    args = ap.parse_args()
    with open(args.csv, newline='') as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise SystemExit('No completed/presented frames in CSV')
    print(f"mode={rows[0].get('mode')} refresh={rows[0].get('refresh_hz')} Hz drawable_count={rows[0].get('drawable_count')} rows={len(rows)}")
    summarize(rows, 'next_drawable_wait_ms', 'nextDrawable wait')
    summarize(rows, 'gpu_duration_ms', 'GPU duration')
    summarize(rows, 'callback_to_presented_ms', 'callback -> presented')
    summarize(rows, 'commit_to_presented_ms', 'commit -> presented')
    summarize(rows, 'gpu_end_to_presented_ms', 'GPU end -> presented')
    summarize(rows, 'requested_to_presented_ms', 'requested -> presented')
    summarize(rows, 'metal_target_to_presented_ms', 'Metal target -> presented')

    presented = values(rows, 'presented_time_s')
    if len(presented) > 1:
        intervals = [(b-a)*1000.0 for a,b in zip(presented,presented[1:]) if b>a]
        if intervals:
            print(f"{'presented interval':30s} n={len(intervals):6d}  median={fmt(statistics.median(intervals)):>8s} ms  p95={fmt(q(intervals,.95)):>8s}  p99={fmt(q(intervals,.99)):>8s}")


if __name__ == '__main__':
    main()
