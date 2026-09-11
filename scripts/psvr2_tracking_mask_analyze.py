#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Measure how Sense led_blink bits affect stationary PSVR2 mode-4 images."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np

MODE4_CAMERA_PLANES = {4: {0: 0, 1: 1}, 5: {0: 2, 1: 3}}


def segment_for_time(timestamp_ns: int, segments: list[dict], margin_ns: int) -> dict | None:
    for segment in segments:
        if int(segment["start_monotonic_ns"]) + margin_ns <= timestamp_ns:
            if timestamp_ns <= int(segment["end_monotonic_ns"]) - margin_ns:
                return segment
    return None


def read_segments(path: Path) -> list[dict]:
    with path.open(newline="") as file:
        return list(csv.DictReader(file))


def read_images(capture_dir: Path, segments: list[dict], margin_ns: int) -> dict[int, dict[str, list[np.ndarray]]]:
    grouped: dict[int, dict[str, list[np.ndarray]]] = defaultdict(lambda: defaultdict(list))
    for csv_path in sorted(capture_dir.glob("*mode-04-packets.csv")):
        with csv_path.open(newline="") as file:
            for row in csv.DictReader(file):
                if not row.get("decoded_files"):
                    continue
                segment = segment_for_time(int(row["host_monotonic_ns"]), segments, margin_ns)
                if segment is None:
                    continue
                camera_set = int(row["camera_set"])
                planes = MODE4_CAMERA_PLANES.get(camera_set, {})
                for name in row["decoded_files"].split(";"):
                    if "-plane" not in name:
                        continue
                    plane = int(name.rsplit("-plane", 1)[1].split(".", 1)[0])
                    camera = planes.get(plane)
                    if camera is None:
                        continue
                    image = cv2.imread(str(capture_dir / name), cv2.IMREAD_GRAYSCALE)
                    if image is not None:
                        grouped[camera][segment["label"]].append(image)
    return grouped


def compact_bright_centroids(difference: np.ndarray, threshold: int) -> list[list[float]]:
    count, _, stats, centroids = cv2.connectedComponentsWithStats((difference >= threshold).astype(np.uint8), 8)
    return [
        [float(centroids[i, 0]), float(centroids[i, 1])]
        for i in range(1, count)
        if 2 <= int(stats[i, cv2.CC_STAT_AREA]) <= 400
        and int(stats[i, cv2.CC_STAT_WIDTH]) <= 40
        and int(stats[i, cv2.CC_STAT_HEIGHT]) <= 40
    ]


def median_image(images: list[np.ndarray]) -> np.ndarray | None:
    if not images:
        return None
    return np.median(np.stack(images), axis=0).astype(np.uint8)


def analyze(capture_dir: Path, manifest_path: Path, threshold: int, margin_ms: int) -> dict:
    segments = read_segments(manifest_path)
    grouped = read_images(capture_dir, segments, margin_ms * 1_000_000)
    cameras = []
    observed_bits = set()
    for camera in range(4):
        labels = grouped.get(camera, {})
        off = median_image(labels.get("all_off", []))
        per_bit = []
        for bit in range(17):
            label = f"bit_{bit:02d}"
            image = median_image(labels.get(label, []))
            centroids = []
            if off is not None and image is not None:
                difference = cv2.subtract(image, off)
                centroids = compact_bright_centroids(difference, threshold)
            if centroids:
                observed_bits.add(bit)
            per_bit.append(
                {
                    "bit": bit,
                    "frame_count": len(labels.get(label, [])),
                    "bright_component_count": len(centroids),
                    "centroids_px": centroids,
                }
            )

        all_on = median_image(labels.get("all_on", []))
        all_on_centroids = []
        if off is not None and all_on is not None:
            all_on_centroids = compact_bright_centroids(cv2.subtract(all_on, off), threshold)
        cameras.append(
            {
                "camera": camera,
                "off_frame_count": len(labels.get("all_off", [])),
                "all_on_frame_count": len(labels.get("all_on", [])),
                "all_on_bright_component_count": len(all_on_centroids),
                "all_on_centroids_px": all_on_centroids,
                "bits": per_bit,
            }
        )

    bits_with_multiple_components = sorted(
        {
            entry["bit"]
            for camera in cameras
            for entry in camera["bits"]
            if entry["bright_component_count"] > 1
        }
    )
    return {
        "format": "psvr2-sense-led-mask-analysis-v1",
        "capture_dir": str(capture_dir),
        "mask_manifest": str(manifest_path),
        "threshold": threshold,
        "segment_edge_margin_ms": margin_ms,
        "observed_bits": sorted(observed_bits),
        "bits_with_multiple_components_in_one_camera": bits_with_multiple_components,
        "mask_semantics_status": "consistent_with_individual_bits"
        if len(observed_bits) >= 4 and not bits_with_multiple_components
        else "inconclusive_or_grouped",
        "cameras": cameras,
        "note": "This first experiment tests mask semantics only. Bit-to-LED-model identity still requires geometric validation.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("mask_manifest", type=Path)
    parser.add_argument("--threshold", type=int, default=40)
    parser.add_argument("--segment-edge-margin-ms", type=int, default=100)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.threshold <= 255:
        raise SystemExit("--threshold must be between 1 and 255")
    if args.segment_edge_margin_ms < 0:
        raise SystemExit("--segment-edge-margin-ms must be non-negative")
    result = analyze(args.capture_dir, args.mask_manifest, args.threshold, args.segment_edge_margin_ms)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(
        f"Observed {len(result['observed_bits'])}/17 candidate bits; "
        f"mask semantics={result['mask_semantics_status']}"
    )
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
