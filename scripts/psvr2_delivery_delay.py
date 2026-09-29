#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""
Measure how late PS VR2 IMU and SLAM samples reach monado-service.

Reads the service-side traces written with PSVR2_TIMING_TRACE=1
(monado_psvr2_<PID>_imu.csv and monado_psvr2_<PID>_slam.csv) and reports, per
run:

- delivery delay: host receipt time minus device time, relative to the
  lower envelope of that difference over a rolling window. The envelope
  cancels the fixed clock offset and slow drift, so a stall or a throttled
  reader thread shows up in full;
- arrival gaps: the longest intervals between consecutive receipts;
- if monado_psvr2_<PID>_compositor_rt.csv is present, the same figures split
  by whether the service's compositor thread was throttled (priority 4 or
  below, i.e. Game Mode backgrounding the service) at the time. The traces
  share the host monotonic clock;
- absorbed delay (IMU): how far the driver's own VTS-to-host mapping has
  moved above the envelope. The driver maps with an exponential filter that
  follows receipt times within about 80 ms, so sustained delivery delay is
  absorbed into the mapping and becomes pose lag without showing up in the
  driver's "mapped" columns.

Usage:
    psvr2_delivery_delay.py [--window-s 5] [--skip-s 5] [--bucket-s 10]
                            PREFIX [PREFIX ...]

PREFIX is the trace path without the _imu.csv / _slam.csv suffix, for example
/tmp/psvr2-trace/monado_psvr2_12345. Pass the non-Game Mode run first to use
it as the baseline.
"""

import argparse
import bisect
import collections
import csv
import os
import sys


def read_columns(path, time_col, ref_col, extra_cols=()):
    """Return a list of (host_ns, ref_ns, *extra) tuples, sorted by host time."""
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                row = [int(r[time_col]), int(r[ref_col])]
                row.extend(int(r[c]) for c in extra_cols)
            except (KeyError, ValueError):
                continue
            rows.append(tuple(row))
    rows.sort(key=lambda row: row[0])
    return rows


def rolling_min(times_ns, values, window_ns):
    """Trailing rolling minimum of values over window_ns, O(n)."""
    out = []
    dq = collections.deque()  # indices with increasing values
    for i, (t, v) in enumerate(zip(times_ns, values)):
        while dq and values[dq[-1]] >= v:
            dq.pop()
        dq.append(i)
        while times_ns[dq[0]] < t - window_ns:
            dq.popleft()
        out.append(values[dq[0]])
    return out


def pct(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    i = int(round(p / 100.0 * (len(sorted_vals) - 1)))
    return sorted_vals[min(max(i, 0), len(sorted_vals) - 1)]


def summarize(label, values_ms):
    s = sorted(values_ms)
    return "{:<22} n={:<7} median={:7.2f} p95={:7.2f} p99={:7.2f} max={:8.2f} ms".format(
        label, len(s), pct(s, 50), pct(s, 95), pct(s, 99), s[-1] if s else float("nan")
    )


def load_throttle_timeline(path):
    """(times_ns, throttled) from compositor_rt.csv; throttled = priority <= 4."""
    times, states = [], []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                t = int(r["sample_ns"])
                pri = int(r["cur_priority"])
            except (KeyError, ValueError):
                continue
            if pri <= 0:  # not sampled in detail
                continue
            times.append(t)
            states.append(pri <= 4)
    order = sorted(range(len(times)), key=lambda i: times[i])
    return [times[i] for i in order], [states[i] for i in order]


def throttled_at(timeline, t):
    times, states = timeline
    i = bisect.bisect_right(times, t) - 1
    if i < 0:
        return None
    # A sample more than 1 s old says nothing about now.
    if t - times[i] > 1_000_000_000:
        return None
    return states[i]


def print_split(kind, timeline, times, delay):
    split = {True: [], False: []}
    for t, d in zip(times, delay):
        state = throttled_at(timeline, t)
        if state is not None:
            split[state].append(d)
    for state, label in ((False, "not throttled"), (True, "throttled (GM)")):
        if split[state]:
            print("  {:<4} ".format(kind) + summarize("delay, " + label, split[state]))


def gaps_ms(times_ns):
    return [(b - a) / 1e6 for a, b in zip(times_ns, times_ns[1:])]


def analyze(prefix, window_s, skip_s, bucket_s):
    imu_path = prefix + "_imu.csv"
    slam_path = prefix + "_slam.csv"
    window_ns = int(window_s * 1e9)
    print("== {}".format(os.path.basename(prefix)))

    timeline = None
    rt_path = prefix + "_compositor_rt.csv"
    if os.path.exists(rt_path):
        timeline = load_throttle_timeline(rt_path)
        if timeline[0]:
            throttled = sum(1 for x in timeline[1] if x)
            print("  compositor_rt: {} samples, {:.1f} % at priority <= 4".format(
                len(timeline[1]), 100.0 * throttled / len(timeline[1])))
            # Throttled periods, for reference.
            spans, start = [], None
            for t, st in zip(*timeline):
                if st and start is None:
                    start = t
                elif not st and start is not None:
                    spans.append((start, t))
                    start = None
            if start is not None:
                spans.append((start, timeline[0][-1]))
            results_origin = timeline[0][0]
            print("  throttled spans (s from first compositor sample): " + ", ".join(
                "{:.1f}-{:.1f}".format((a - results_origin) / 1e9, (b - results_origin) / 1e9) for a, b in spans))
        else:
            timeline = None

    results = {}

    if os.path.exists(imu_path):
        imu = read_columns(imu_path, "host_estimated_sample_ns", "vts_ns", ("vts_mapped_host_ns",))
        if imu:
            t = [r[0] for r in imu]
            diff = [r[0] - r[1] for r in imu]
            floor = rolling_min(t, diff, window_ns)
            start = t[0] + int(skip_s * 1e9)
            delay, absorbed, times = [], [], []
            for (host, vts, mapped), d, fl in zip(imu, diff, floor):
                if host < start:
                    continue
                delay.append((d - fl) / 1e6)
                # Where the driver believes the sample happened, versus the
                # envelope's estimate of when it really happened.
                absorbed.append((mapped - (vts + fl)) / 1e6)
                times.append(host)
            print("  IMU  " + summarize("delivery delay", delay))
            print("  IMU  " + summarize("absorbed into mapping", absorbed))
            g = sorted(gaps_ms([x for x in t if x >= start]))
            print("  IMU  arrival gaps: p99={:.2f} max={:.2f} ms, gaps >5 ms: {}".format(
                pct(g, 99), g[-1] if g else float("nan"), sum(1 for x in g if x > 5.0)))
            results["imu"] = (times, delay, t[0])
            if timeline:
                print_split("IMU", timeline, times, delay)
    else:
        print("  (no {})".format(imu_path))

    if os.path.exists(slam_path):
        slam = read_columns(slam_path, "host_received_ns", "slam_vts_ns")
        if slam:
            t = [r[0] for r in slam]
            diff = [r[0] - r[1] for r in slam]
            floor = rolling_min(t, diff, window_ns)
            start = t[0] + int(skip_s * 1e9)
            delay = [(d - fl) / 1e6 for (host, _), d, fl in zip(slam, diff, floor) if host >= start]
            times = [host for host, _ in slam if host >= start]
            print("  SLAM " + summarize("delivery delay", delay))
            g = sorted(gaps_ms(times))
            print("  SLAM arrival gaps: median={:.2f} p99={:.2f} max={:.2f} ms, gaps >30 ms: {}".format(
                pct(g, 50), pct(g, 99), g[-1] if g else float("nan"), sum(1 for x in g if x > 30.0)))
            results["slam"] = (times, delay)
            if timeline:
                print_split("SLAM", timeline, times, delay)
    else:
        print("  (no {})".format(slam_path))

    # Per-bucket view, to see when Game Mode engaged.
    if bucket_s > 0 and "imu" in results:
        times, delay, t0 = results["imu"]
        if times:
            # Buckets count from the start of the trace, not from after --skip-s.
            buckets = collections.defaultdict(list)
            for t, d in zip(times, delay):
                buckets[int((t - t0) / (bucket_s * 1e9))].append(d)
            print("  IMU delivery delay by {:g} s bucket (p95 / max ms):".format(bucket_s))
            line = []
            for k in sorted(buckets):
                s = sorted(buckets[k])
                line.append("{:>4.0f}s {:.1f}/{:.1f}".format(k * bucket_s, pct(s, 95), s[-1]))
            for i in range(0, len(line), 6):
                print("    " + "   ".join(line[i:i + 6]))
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefixes", nargs="+", help="trace path prefix, e.g. /tmp/monado_psvr2_12345")
    ap.add_argument("--window-s", type=float, default=5.0, help="lower-envelope window (default 5)")
    ap.add_argument("--skip-s", type=float, default=5.0, help="ignore the first seconds (default 5)")
    ap.add_argument("--bucket-s", type=float, default=10.0, help="per-bucket breakdown, 0 to disable (default 10)")
    args = ap.parse_args()
    for prefix in args.prefixes:
        prefix = prefix[:-8] if prefix.endswith("_imu.csv") else prefix
        prefix = prefix[:-9] if prefix.endswith("_slam.csv") else prefix
        analyze(prefix, args.window_s, args.skip_s, args.bucket_s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
