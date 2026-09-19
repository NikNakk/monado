#!/usr/bin/env python3
"""Summarise Monado Wine D3D11 timing traces without third-party packages."""

from __future__ import annotations

import csv
import math
import statistics
import sys
from pathlib import Path


def percentile(values: list[float], p: float) -> float:
    if not values:
        return math.nan
    values = sorted(values)
    if len(values) == 1:
        return values[0]
    position = (len(values) - 1) * p
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return values[low]
    fraction = position - low
    return values[low] * (1.0 - fraction) + values[high] * fraction


def describe(name: str, values: list[float]) -> None:
    if not values:
        print(f"{name:24s} no data")
        return
    print(
        f"{name:24s} "
        f"p50={percentile(values, 0.50):8.1f} us  "
        f"p95={percentile(values, 0.95):8.1f} us  "
        f"p99={percentile(values, 0.99):8.1f} us  "
        f"max={max(values):8.1f} us  "
        f"mean={statistics.fmean(values):8.1f} us"
    )


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {Path(sys.argv[0]).name} <timing.csv>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))

    if not rows:
        print("Trace contains no frames.", file=sys.stderr)
        return 1

    metrics = [
        "wait_frame_us",
        "producer_wait_us",
        "ipc_commit_us",
        "layer_commit_total_us",
    ]

    print(f"Trace: {path}")
    print(f"Frames: {len(rows)}")
    gpu_values = [int(row.get("gpu_sync", "0") or 0) for row in rows]
    print(f"GPU-sync frames: {sum(gpu_values)} ({100.0 * sum(gpu_values) / len(rows):.1f}%)")
    print()

    for metric in metrics:
        values = [float(row[metric]) for row in rows if row.get(metric)]
        describe(metric, values)

    print()
    total = [float(row["layer_commit_total_us"]) for row in rows if row.get("layer_commit_total_us")]
    for threshold_ms in (1, 2, 4, 8):
        threshold_us = threshold_ms * 1000.0
        count = sum(value >= threshold_us for value in total)
        print(
            f"commit >= {threshold_ms:2d} ms: {count:6d} "
            f"({100.0 * count / len(total):6.2f}%)"
        )

    worst = sorted(
        rows,
        key=lambda row: float(row.get("layer_commit_total_us", "0") or 0),
        reverse=True,
    )[:10]
    print("\nWorst 10 layer commits:")
    for row in worst:
        print(
            f"  frame={row.get('frame_id','?'):>8s} "
            f"gpu={row.get('gpu_sync','0')} "
            f"producer={float(row.get('producer_wait_us','0')):8.1f} us "
            f"ipc={float(row.get('ipc_commit_us','0')):8.1f} us "
            f"total={float(row.get('layer_commit_total_us','0')):8.1f} us"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
