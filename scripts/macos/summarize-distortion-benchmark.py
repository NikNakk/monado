#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Summarize offscreen distortion/composition GPU timings per capture.

Each gpu_ms sample is the average of a batch of dispatches, not an individual
frame. Ratios are paired by round to reduce sensitivity to run ordering.
"""
import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+")
    args = parser.parse_args()
    for path in args.captures:
        groups = defaultdict(dict)
        with path.open(newline="") as source:
            for row in csv.DictReader(source):
                layer_capture = "quads" in row
                key = ((f"projection + {row['quads']} quads (sRGB={row.get('correction', '0')})", "layer", row["width"], row["height"])
                       if layer_capture else
                       (row["scenario"], row["alpha"], row["width"], row["height"]))
                sample = (row["variant"] if layer_capture else row["workgroup"], int(row["round"]))
                if sample in groups[key]:
                    raise ValueError(f"{path}: duplicate sample {key} {sample}")
                value = float(row["gpu_ms"])
                if not 0 < value < float("inf"):
                    raise ValueError(f"{path}: invalid GPU duration {value}")
                groups[key][sample] = value
        if not groups:
            raise ValueError(f"{path}: empty capture (GPU benchmark may have skipped)")
        print(f"\n{path}")
        for (scenario, alpha, width, height), samples in groups.items():
            print(f"  {scenario}, alpha={alpha}, {width}x{height}")
            for group in sorted({g for g, _ in samples}):
                rounds = sorted(r for g, r in samples if g == group)
                values = [samples[group, r] for r in rounds]
                baseline = "baseline" if alpha == "layer" else "8x8"
                ratios = [samples[group, r] / samples[baseline, r] for r in rounds]
                print(f"    {group:6s} median {statistics.median(values):.4f} ms, "
                      f"range {min(values):.4f}–{max(values):.4f}, "
                      f"paired change {(statistics.median(ratios)-1)*100:+.2f}% "
                      f"({len(values)} batches)")


if __name__ == "__main__":
    main()
