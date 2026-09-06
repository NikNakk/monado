#!/usr/bin/env python3
"""
Analyze CAMetalDisplayLink latch timing from one or more MetalLatencyProbe CSVs.

Primary question:
    How early must the GPU finish relative to targetPresentationTimestamp
    for the drawable to hit that target rather than slip by 1+ refreshes?

Examples:
    ./analyze_latch.py /tmp/metal1_delay_*.csv
    ./analyze_latch.py --bin-ms 0.5 /tmp/metal1_delay_*.csv
    ./analyze_latch.py --details-out /tmp/latch_frames.csv \
                       --bins-out /tmp/latch_bins.csv \
                       /tmp/metal1_delay_*.csv
"""

import argparse
import csv
import math
import statistics
import sys
from pathlib import Path


def fnum(row, key):
    try:
        x = float(row[key])
    except (KeyError, TypeError, ValueError):
        return None
    return x if math.isfinite(x) else None


def median(xs):
    return statistics.median(xs) if xs else float("nan")


def percentile(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    pos = (len(s) - 1) * p
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return s[lo]
    return s[lo] * (hi - pos) + s[hi] * (pos - lo)


def estimate_period_ms(rows):
    """
    Estimate the physical refresh period robustly even when the run is
    dropping to 60 Hz.

    Start from refresh_hz as a nominal period, then for each positive
    presented-time interval divide by the nearest integer number of nominal
    refreshes. The median of those base-period estimates is the physical
    refresh period.
    """
    hz = [fnum(r, "refresh_hz") for r in rows]
    hz = [x for x in hz if x and x > 1.0]
    nominal_ms = 1000.0 / median(hz) if hz else 1000.0 / 120.0

    seq_rows = []
    for r in rows:
        seq = fnum(r, "sequence")
        pt = fnum(r, "presented_time_s")
        if seq is not None and pt is not None:
            seq_rows.append((seq, pt))
    seq_rows.sort()

    bases = []
    for (_, a), (_, b) in zip(seq_rows, seq_rows[1:]):
        dt_ms = (b - a) * 1000.0
        if dt_ms <= 0:
            continue
        n = max(1, round(dt_ms / nominal_ms))
        base = dt_ms / n
        # Exclude gross discontinuities while being generous.
        if 0.75 * nominal_ms <= base <= 1.25 * nominal_ms:
            bases.append(base)

    return median(bases) if bases else nominal_ms


def load_file(path):
    with open(path, newline="") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        raise ValueError(f"{path}: no rows")
    return rows


def classify_file(path):
    rows = load_file(path)
    period_ms = estimate_period_ms(rows)

    frames = []
    for r in rows:
        target = fnum(r, "metal_target_presentation_s")
        gpu_end = fnum(r, "gpu_end_s")
        presented = fnum(r, "presented_time_s")
        if target is None or gpu_end is None or presented is None:
            continue

        lead_ms = (target - gpu_end) * 1000.0
        target_delta_ms = (presented - target) * 1000.0

        # Quantize presented-target onto the physical refresh grid.
        miss_refreshes = int(round(target_delta_ms / period_ms))
        # Tiny negative reported presentation differences are normal.
        if miss_refreshes < 0 and abs(target_delta_ms) < 0.25 * period_ms:
            miss_refreshes = 0

        frames.append({
            "file": str(path),
            "sequence": int(float(r.get("sequence", 0))),
            "cpu_delay_ms": fnum(r, "cpu_delay_ms"),
            "gpu_duration_ms": fnum(r, "gpu_duration_ms"),
            "gpu_finish_lead_ms": lead_ms,
            "target_to_presented_ms": target_delta_ms,
            "miss_refreshes": miss_refreshes,
            "hit": 1 if miss_refreshes == 0 else 0,
        })

    return rows, frames, period_ms


def fmt(x, digits=3):
    return "n/a" if x is None or not math.isfinite(x) else f"{x:.{digits}f}"


def pct(n, d):
    return float("nan") if not d else 100.0 * n / d


def print_per_file(summaries):
    print("\nPER-FILE SUMMARY")
    print(
        f"{'file':34s} {'delay':>7s} {'n':>6s} {'period':>8s} "
        f"{'hit%':>7s} {'+1%':>7s} {'+2+%':>7s} {'lead p50':>10s} {'lead p05':>10s}"
    )
    for s in summaries:
        frames = s["frames"]
        n = len(frames)
        delay_vals = [x["cpu_delay_ms"] for x in frames if x["cpu_delay_ms"] is not None]
        leads = [x["gpu_finish_lead_ms"] for x in frames]
        hit = sum(x["miss_refreshes"] == 0 for x in frames)
        one = sum(x["miss_refreshes"] == 1 for x in frames)
        two = sum(x["miss_refreshes"] >= 2 for x in frames)
        name = Path(s["path"]).name
        if len(name) > 34:
            name = "…" + name[-33:]
        print(
            f"{name:34s} "
            f"{fmt(median(delay_vals),1):>7s} "
            f"{n:6d} "
            f"{fmt(s['period_ms']):>8s} "
            f"{fmt(pct(hit,n),1):>7s} "
            f"{fmt(pct(one,n),1):>7s} "
            f"{fmt(pct(two,n),1):>7s} "
            f"{fmt(median(leads)):>10s} "
            f"{fmt(percentile(leads,.05)):>10s}"
        )


def bin_frames(frames, bin_ms, min_bin_n):
    valid = [x for x in frames if math.isfinite(x["gpu_finish_lead_ms"])]
    if not valid:
        return []

    lo = math.floor(min(x["gpu_finish_lead_ms"] for x in valid) / bin_ms) * bin_ms
    hi = math.ceil(max(x["gpu_finish_lead_ms"] for x in valid) / bin_ms) * bin_ms

    out = []
    edge = lo
    # Add a tiny epsilon so the top edge is included.
    while edge <= hi + 1e-12:
        upper = edge + bin_ms
        b = [
            x for x in valid
            if edge <= x["gpu_finish_lead_ms"] < upper
        ]
        if b:
            n = len(b)
            hit = sum(x["miss_refreshes"] == 0 for x in b)
            one = sum(x["miss_refreshes"] == 1 for x in b)
            two = sum(x["miss_refreshes"] >= 2 for x in b)
            out.append({
                "lead_lo_ms": edge,
                "lead_hi_ms": upper,
                "lead_mid_ms": edge + bin_ms / 2.0,
                "n": n,
                "hit_pct": pct(hit, n),
                "plus1_pct": pct(one, n),
                "plus2plus_pct": pct(two, n),
                "show": n >= min_bin_n,
            })
        edge = upper
    return out


def print_bins(bins):
    print("\nGPU FINISH LEAD -> PRESENTATION OUTCOME")
    print("(positive lead = GPU finished before targetPresentationTimestamp)")
    print(f"{'lead before target':>20s} {'n':>6s} {'hit target':>12s} {'+1 refresh':>12s} {'+2+':>9s}")
    for b in bins:
        if not b["show"]:
            continue
        rng = f"{b['lead_lo_ms']:5.1f}..{b['lead_hi_ms']:5.1f} ms"
        print(
            f"{rng:>20s} {b['n']:6d} "
            f"{b['hit_pct']:11.1f}% {b['plus1_pct']:11.1f}% {b['plus2plus_pct']:8.1f}%"
        )


def cumulative_thresholds(frames, step_ms=0.1, min_n=100):
    """
    For each threshold T, consider every frame whose GPU finished at least
    T ms before the target. Return the smallest lead producing selected
    cumulative target-hit rates.
    """
    valid = [x for x in frames if math.isfinite(x["gpu_finish_lead_ms"])]
    if not valid:
        return {}

    lo = math.floor(min(x["gpu_finish_lead_ms"] for x in valid) / step_ms) * step_ms
    hi = math.ceil(max(x["gpu_finish_lead_ms"] for x in valid) / step_ms) * step_ms

    points = []
    t = lo
    while t <= hi + 1e-12:
        subset = [x for x in valid if x["gpu_finish_lead_ms"] >= t]
        if len(subset) >= min_n:
            rate = sum(x["hit"] for x in subset) / len(subset)
            points.append((t, len(subset), rate))
        t += step_ms

    result = {}
    for wanted in (0.50, 0.90, 0.95, 0.99):
        candidates = [(t, n, r) for t, n, r in points if r >= wanted]
        if candidates:
            # Smallest required lead meeting the cumulative criterion.
            result[wanted] = min(candidates, key=lambda x: x[0])
    return result


def print_thresholds(thresholds):
    print("\nCUMULATIVE SAFE-LEAD ESTIMATE")
    print("For each T: among all frames with GPU completion >= T ms before target:")
    for wanted in (0.50, 0.90, 0.95, 0.99):
        if wanted not in thresholds:
            print(f"  {wanted*100:4.0f}% target-hit rate: not reached with enough samples")
            continue
        t, n, rate = thresholds[wanted]
        print(
            f"  {wanted*100:4.0f}% target-hit rate: lead >= {t:5.2f} ms "
            f"(n={n}, observed {rate*100:5.2f}%)"
        )


def write_details(path, frames):
    fields = [
        "file", "sequence", "cpu_delay_ms", "gpu_duration_ms",
        "gpu_finish_lead_ms", "target_to_presented_ms",
        "miss_refreshes", "hit",
    ]
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(frames)


def write_bins(path, bins):
    fields = [
        "lead_lo_ms", "lead_hi_ms", "lead_mid_ms", "n",
        "hit_pct", "plus1_pct", "plus2plus_pct",
    ]
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(bins)


def main():
    ap = argparse.ArgumentParser(
        description="Measure CAMetalDisplayLink target-hit probability vs GPU completion lead."
    )
    ap.add_argument("csv", nargs="+", help="MetalLatencyProbe CSV file(s)")
    ap.add_argument("--bin-ms", type=float, default=0.5,
                    help="GPU lead-time bin width in ms (default: 0.5)")
    ap.add_argument("--min-bin-n", type=int, default=20,
                    help="minimum frames required to print a bin (default: 20)")
    ap.add_argument("--threshold-step-ms", type=float, default=0.1,
                    help="step for cumulative safe-lead calculation (default: 0.1 ms)")
    ap.add_argument("--threshold-min-n", type=int, default=100,
                    help="minimum samples for cumulative threshold (default: 100)")
    ap.add_argument("--details-out",
                    help="optional CSV containing one classified row per frame")
    ap.add_argument("--bins-out",
                    help="optional CSV containing the binned outcome table")
    args = ap.parse_args()

    if args.bin_ms <= 0 or args.threshold_step_ms <= 0:
        ap.error("bin/threshold step must be > 0")

    all_frames = []
    summaries = []

    for raw_path in args.csv:
        path = Path(raw_path)
        try:
            rows, frames, period_ms = classify_file(path)
        except Exception as e:
            print(f"warning: {path}: {e}", file=sys.stderr)
            continue

        if not frames:
            print(f"warning: {path}: no rows with target, GPU-end, and presented timestamps",
                  file=sys.stderr)
            continue

        summaries.append({
            "path": str(path),
            "rows": rows,
            "frames": frames,
            "period_ms": period_ms,
        })
        all_frames.extend(frames)

    if not all_frames:
        raise SystemExit("No usable frames found")

    print_per_file(summaries)

    periods = [s["period_ms"] for s in summaries]
    pooled_period = median(periods)
    print(
        f"\nPooled: {len(all_frames)} frames from {len(summaries)} files; "
        f"typical physical refresh period ≈ {pooled_period:.6f} ms "
        f"({1000.0/pooled_period:.4f} Hz)"
    )

    bins = bin_frames(all_frames, args.bin_ms, args.min_bin_n)
    print_bins(bins)

    thresholds = cumulative_thresholds(
        all_frames,
        step_ms=args.threshold_step_ms,
        min_n=args.threshold_min_n,
    )
    print_thresholds(thresholds)

    # Useful raw distribution around the boundary.
    hits = [x["gpu_finish_lead_ms"] for x in all_frames if x["hit"]]
    misses = [x["gpu_finish_lead_ms"] for x in all_frames if not x["hit"]]
    print("\nLEAD-TIME DISTRIBUTIONS")
    if hits:
        print(
            "  hits:   "
            f"p05={percentile(hits,.05):.3f}  "
            f"median={median(hits):.3f}  "
            f"p95={percentile(hits,.95):.3f} ms"
        )
    if misses:
        print(
            "  misses: "
            f"p05={percentile(misses,.05):.3f}  "
            f"median={median(misses):.3f}  "
            f"p95={percentile(misses,.95):.3f} ms"
        )

    if args.details_out:
        write_details(args.details_out, all_frames)
        print(f"\nWrote frame details: {args.details_out}")

    if args.bins_out:
        write_bins(args.bins_out, bins)
        print(f"Wrote binned results: {args.bins_out}")


if __name__ == "__main__":
    main()
