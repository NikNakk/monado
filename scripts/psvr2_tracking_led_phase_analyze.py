#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Measure static Sense LED brightness by commanded mode-4 pulse phase."""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np

SWEEP_LINE = re.compile(
    r"LED_PHASE_SWEEP side=([LR]) seq=(\d+) step=(-?\d+) phase_us=(\d+) pulse_us=(\d+) off=(\d+)"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_stages(log: Path, hand: str) -> list[dict]:
    side = "L" if hand == "left" else "R"
    stages = []
    for line in log.read_text(errors="replace").splitlines():
        match = SWEEP_LINE.search(line)
        if match is None or match[1] != side:
            continue
        stages.append(
            {
                "start_sequence": int(match[2]),
                "step": int(match[3]),
                "commanded_phase_us": int(match[4]),
                "pulse_us": int(match[5]),
                "off": bool(int(match[6])),
            }
        )
    if not stages or stages[0]["step"] != -1 or stages[-1]["step"] != 134:
        raise ValueError("log does not contain a complete off/450us/2100us/off sweep")
    if [stage["step"] for stage in stages] != [-1, *range(135)]:
        raise ValueError("sweep stage order is incomplete or duplicated")
    return stages


def load_image(path: Path) -> np.ndarray:
    image = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if image is None or image.shape != (508, 512):
        raise ValueError(f"missing or unexpected mode-4 image: {path}")
    return image[:, :508]


def analyze_camera(capture: Path, camera: int, stages: list[dict], roi: tuple[int, int, int, int],
                   threshold: int, settle_sequences: int) -> dict:
    manifest = capture / f"camera{camera}.csv"
    with manifest.open(newline="") as file:
        rows = list(csv.DictReader(file))
    starts = [stage["start_sequence"] for stage in stages]
    images = []
    combined_digest = hashlib.sha256()
    for row in rows:
        path = capture / row["file"]
        combined_digest.update(row["file"].encode())
        combined_digest.update(bytes.fromhex(sha256(path)))
        sequence = int(row["source_sequence"])
        index = bisect.bisect_right(starts, sequence) - 1
        if index < 0 or sequence - starts[index] < settle_sequences:
            continue
        images.append((stages[index]["step"], sequence, path))

    # Use only the initial LEDs-off stage to establish the scene background.
    off = [load_image(path) for step, _, path in images if step == -1]
    if len(off) < 3:
        raise ValueError(f"camera {camera} has too few initial LEDs-off images")
    background = np.median(np.stack(off), axis=0).astype(np.uint8)
    x0, y0, x1, y1 = roi
    if not (0 <= x0 < x1 <= 508 and 0 <= y0 < y1 <= 508):
        raise ValueError(f"invalid camera {camera} ROI: {roi}")
    reference = background[y0:y1, x0:x1]

    grouped: dict[int, list[dict]] = defaultdict(list)
    for step, sequence, path in images:
        image = load_image(path)[y0:y1, x0:x1]
        difference = cv2.subtract(image, reference)
        bright = difference >= threshold
        grouped[step].append(
            {
                "sequence": sequence,
                "bright_pixels": int(np.count_nonzero(bright)),
                "brightness_sum": int(difference[bright].sum()),
            }
        )
    results = []
    for stage in stages:
        values = grouped[stage["step"]]
        results.append(
            {
                **stage,
                "image_count": len(values),
                "median_bright_pixels": float(np.median([v["bright_pixels"] for v in values])) if values else None,
                "median_brightness_sum": float(np.median([v["brightness_sum"] for v in values])) if values else None,
                "samples": values,
            }
        )
    return {
        "camera": camera,
        "roi_xyxy_active_pixels": list(roi),
        "manifest_sha256": sha256(manifest),
        "ordered_image_content_sha256": combined_digest.hexdigest(),
        "manifest_image_count": len(rows),
        "initial_off_image_count": len(off),
        "stages": results,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("log", type=Path)
    parser.add_argument("--hand", choices=("left", "right"), default="left")
    parser.add_argument("--roi", action="append", nargs=4, type=int, metavar=("X0", "Y0", "X1", "Y1"),
                        required=True, help="one native active-image ROI per camera, in camera order")
    parser.add_argument("--threshold", type=int, default=80)
    parser.add_argument("--settle-sequences", type=int, default=6)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if len(args.roi) != 4 or not 1 <= args.threshold <= 255 or args.settle_sequences < 0:
        parser.error("provide four --roi values, threshold 1..255, and nonnegative settling")
    stages = read_stages(args.log, args.hand)
    cameras = [analyze_camera(args.capture, camera, stages, tuple(args.roi[camera]), args.threshold,
                              args.settle_sequences) for camera in range(4)]
    result = {
        "format": "psvr2-mode4-static-led-phase-analysis-v1",
        "capture": str(args.capture.resolve()),
        "log": str(args.log.resolve()),
        "log_sha256": sha256(args.log),
        "hand": args.hand,
        "active_image_size": [508, 508],
        "threshold_above_initial_off_median": args.threshold,
        "settle_sequences_after_stage_change": args.settle_sequences,
        "note": "Commanded phase is not a direct measurement of emitted-light phase. ROI coordinates were selected from the static sweep's off-subtracted maximum images.",
        "cameras": cameras,
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    for camera in cameras:
        print(f"camera {camera['camera']}: {camera['manifest_image_count']} images; ROI {camera['roi_xyxy_active_pixels']}")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
