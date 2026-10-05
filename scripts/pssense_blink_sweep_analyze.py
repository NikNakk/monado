#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Analyse a PSSENSE_LED_BLINK_SWEEP session: per led_blink value, which camera frames show the ring.

Usage:
    pssense_blink_sweep_analyze.py SESSION_DIR [--replay PATH] [--side L|R] [--settle 0.3]

The session must hold run.log (with LED_BLINK_SWEEP markers) and constellation.ctd. Run it with one controller
awake so every bright blob belongs to it. A frame counts as lit when at least --min-cameras cameras each see at
least --min-blobs more blobs than that camera's background: the lowest count seen in at least 0.3% of frames,
which the LED-off baselines supply.

For every step it prints the lit fraction, the strongest repeat period (1-40 frames, by autocorrelation) and the
frames folded modulo 32 from the step's first frame ('#' lit in most cycles, '+' in some, '.' never), next to the
value's bits in two candidate orders (byte 0 bit 0 first, and byte 0 bit 7 first), for comparison. The fold's
rotation is arbitrary: the controller's slot origin is not known.
"""

import argparse
import csv
import re
import statistics
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

DEFAULT_REPLAY = Path(__file__).resolve().parent.parent / "build-macos-sense-rel/src/xrt/tracking/constellation/constellation_replay"


def bits(value, msb_first):
    out = []
    for byte in bytes.fromhex(value):
        for i in range(8):
            out.append((byte >> (7 - i if msb_first else i)) & 1)
    return "".join("#" if b else "." for b in out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("session", type=Path)
    ap.add_argument("--replay", type=Path, default=DEFAULT_REPLAY)
    ap.add_argument("--side", default=None)
    ap.add_argument("--settle", type=float, default=0.3, help="seconds skipped after each step's latch")
    ap.add_argument("--min-blobs", type=int, default=3)
    ap.add_argument("--min-cameras", type=int, default=1)
    args = ap.parse_args()

    steps = []
    for line in open(args.session / "run.log", errors="replace"):
        m = re.search(r"LED_BLINK_SWEEP side=(\w) event=(\w+)(.*)", line)
        if not m or (args.side and m.group(1) != args.side):
            continue
        d = dict(re.findall(r"(\w+)=(\S+)", m.group(3)))
        steps.append((int(d["exposure_ns"]), d.get("value", "end"), d.get("broad", "-")))
    if not steps:
        sys.exit("no LED_BLINK_SWEEP markers in run.log")

    with tempfile.TemporaryDirectory() as tmp:
        blobs_csv = Path(tmp) / "blobs.csv"
        subprocess.run([str(args.replay), str(args.session / "constellation.ctd"), "--blobs-csv", str(blobs_csv)],
                       check=True, capture_output=True)
        counts = defaultdict(lambda: defaultdict(int))
        for r in csv.DictReader(open(blobs_csv)):
            counts[int(r["timestamp_ns"])][int(r["camera"])] += 1
    frames = sorted(counts)
    cameras = sorted({c for f in counts.values() for c in f})

    # The lowest count seen in at least 0.3% of frames: a percentile fails when the ring is lit almost throughout.
    background = {}
    for c in cameras:
        hist = defaultdict(int)
        for t in frames:
            hist[counts[t].get(c, 0)] += 1
        background[c] = min(v for v, n in hist.items() if n >= 0.003 * len(frames))

    def lit(t):
        return sum(1 for c in cameras if counts[t].get(c, 0) - background[c] >= args.min_blobs) >= args.min_cameras

    print(f"{len(frames)} frames, cameras {cameras}, background {background}")
    for i, (start, value, broad) in enumerate(steps):
        if value == "end":
            continue
        end = steps[i + 1][0] if i + 1 < len(steps) else frames[-1]
        window = [t for t in frames if start + args.settle * 1e9 <= t < end]
        if len(window) < 40:
            print(f"{value}: too few frames ({len(window)})")
            continue
        seq = [1 if lit(t) else 0 for t in window]
        frac = sum(seq) / len(seq)
        best, best_score = 0, -1.0
        mean = frac
        var = sum((x - mean) ** 2 for x in seq) / len(seq)
        if var > 0:
            for lag in range(1, 41):
                c = sum((seq[k] - mean) * (seq[k + lag] - mean) for k in range(len(seq) - lag)) / (len(seq) - lag) / var
                if c > best_score:
                    best, best_score = lag, c
        fold = defaultdict(list)
        for k, x in enumerate(seq):
            fold[k % 32].append(x)
        pattern = "".join("#" if statistics.mean(fold[k]) > 0.75 else ("+" if statistics.mean(fold[k]) > 0.1 else ".")
                          for k in range(32))
        period = f"{best} (r={best_score:.2f})" if var > 0 else "constant"
        print(f"{value} broad={broad}: {len(seq)} frames, lit {100 * frac:.0f}%, period {period}")
        print(f"   folded /32 : {pattern}")
        print(f"   bits lsb1st: {bits(value, False)}")
        print(f"   bits msb1st: {bits(value, True)}")


if __name__ == "__main__":
    main()
