#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Assess PSVR2 ChArUco calibration coverage from detections + manifest."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

CAMERA_COUNT = 4


def load_rows(path: Path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("dataset", type=Path)
    p.add_argument("--min-corners", type=int, default=8)
    args = p.parse_args()

    detections = load_rows(args.dataset / "charuco-detections.csv")
    manifest = load_rows(args.dataset / "manifest.csv")
    if not detections:
        raise SystemExit("No ChArUco detections found")

    counts: dict[tuple[int, int], int] = defaultdict(int)
    xs: dict[int, list[float]] = defaultdict(list)
    ys: dict[int, list[float]] = defaultdict(list)
    for row in detections:
        camera = int(row["camera"])
        set_index = int(row["set_index"])
        counts[(camera, set_index)] += 1
        xs[camera].append(float(row["x"]))
        ys[camera].append(float(row["y"]))

    print(f"manifest sets: {len(manifest)}")
    print(f"corner observations: {len(detections)}")

    for camera in range(CAMERA_COUNT):
        per_frame = [n for (c, _), n in counts.items() if c == camera]
        strong = [n for n in per_frame if n >= args.min_corners]
        if per_frame:
            print(
                f"camera {camera}: detected={len(per_frame)} strong={len(strong)} "
                f"mean_corners={sum(per_frame)/len(per_frame):.1f} "
                f"coverage_x={min(xs[camera]):.1f}..{max(xs[camera]):.1f} "
                f"coverage_y={min(ys[camera]):.1f}..{max(ys[camera]):.1f}"
            )
        else:
            print(f"camera {camera}: no detections")

    set_ids = sorted({s for _, s in counts})
    strong_per_set = {
        s: sum(counts.get((camera, s), 0) >= args.min_corners for camera in range(CAMERA_COUNT))
        for s in set_ids
    }
    print(
        "strong synchronized sets: "
        + ", ".join(
            f">={n} cameras={sum(v >= n for v in strong_per_set.values())}"
            for n in range(1, CAMERA_COUNT + 1)
        )
    )

    for a in range(CAMERA_COUNT):
        for b in range(a + 1, CAMERA_COUNT):
            both = sum(
                counts.get((a, s), 0) >= args.min_corners
                and counts.get((b, s), 0) >= args.min_corners
                for s in set_ids
            )
            print(f"pair {a}-{b}: strong synchronized observations={both}")

    if manifest:
        valid = sum(int(r.get("slam_pose_valid", "0") or 0) != 0 for r in manifest)
        exact = sum(int(float(r.get("slam_nearest_delta_us", "-1") or -1)) == 0 for r in manifest)
        print(f"SLAM-associated sets: valid={valid}/{len(manifest)} exact_vts={exact}/{len(manifest)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
