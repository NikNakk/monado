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
LED_MASK_BIT_COUNT = 32


def segment_for_time(
    timestamp_ns: int,
    segments: list[dict],
    margin_ns: int,
    start_key: str = "start_monotonic_ns",
    end_key: str = "end_monotonic_ns",
) -> dict | None:
    for segment in segments:
        if int(segment[start_key]) + margin_ns <= timestamp_ns:
            if timestamp_ns <= int(segment[end_key]) - margin_ns:
                return segment
    return None


def read_segments(path: Path) -> tuple[list[dict], str]:
    with path.open(newline="") as file:
        segments = list(csv.DictReader(file))
    if segments and segments[0].get("start_realtime_ns"):
        return segments, "recorded_realtime"

    # v1 manifests recorded the C CLOCK_MONOTONIC domain, which is not the
    # same epoch as Python's monotonic_ns() on macOS. Recover those captures
    # from the manifest close time and image modification times.
    if segments:
        offset = path.stat().st_mtime_ns - int(segments[-1]["end_monotonic_ns"])
        for segment in segments:
            segment["start_realtime_ns"] = str(int(segment["start_monotonic_ns"]) + offset)
            segment["end_realtime_ns"] = str(int(segment["end_monotonic_ns"]) + offset)
    return segments, "realtime_estimated_from_manifest_mtime"


def read_images(capture_dir: Path, segments: list[dict], margin_ns: int) -> dict[int, dict[str, list[np.ndarray]]]:
    grouped: dict[int, dict[str, list[np.ndarray]]] = defaultdict(lambda: defaultdict(list))
    for csv_path in sorted(capture_dir.glob("*mode-04-packets.csv")):
        with csv_path.open(newline="") as file:
            for row in csv.DictReader(file):
                if not row.get("decoded_files"):
                    continue
                names = row["decoded_files"].split(";")
                if row.get("host_realtime_ns"):
                    timestamp_ns = int(row["host_realtime_ns"])
                else:
                    timestamp_ns = (capture_dir / names[0]).stat().st_mtime_ns
                segment = segment_for_time(
                    timestamp_ns, segments, margin_ns, "start_realtime_ns", "end_realtime_ns"
                )
                if segment is None:
                    continue
                camera_set = int(row["camera_set"])
                planes = MODE4_CAMERA_PLANES.get(camera_set, {})
                for name in names:
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


def centroid_match_fraction(points: list[list[float]], reference: list[list[float]], max_distance_px: float = 4.0) -> float:
    if not points or not reference:
        return 0.0
    available = set(range(len(reference)))
    matched = 0
    for point in points:
        candidates = sorted(
            (float(np.linalg.norm(np.asarray(point) - np.asarray(reference[index]))), index) for index in available
        )
        if candidates and candidates[0][0] <= max_distance_px:
            matched += 1
            available.remove(candidates[0][1])
    return matched / len(points)


def matching_component_count(points: list[list[float]], reference: list[list[float]]) -> int:
    return round(len(points) * centroid_match_fraction(points, reference))


def median_image(images: list[np.ndarray]) -> np.ndarray | None:
    if not images:
        return None
    return np.median(np.stack(images), axis=0).astype(np.uint8)


def classify_led_blink_semantics(
    observed_bits: set[int], bits_with_multiple_matching_components: list[int], bits_with_temporal_toggle: list[int]
) -> str:
    if bits_with_temporal_toggle:
        return "temporal_waveform_supported"
    if bits_with_multiple_matching_components:
        return "grouped_or_shared_mask_supported"
    if observed_bits:
        return "spatial_mask_supported"
    return "inconclusive"


def analyze(capture_dir: Path, manifest_path: Path, threshold: int, margin_ms: int) -> dict:
    segments, timing_basis = read_segments(manifest_path)
    grouped = read_images(capture_dir, segments, margin_ms * 1_000_000)
    cameras = []
    observed_bits: set[int] = set()
    for camera in range(4):
        labels = grouped.get(camera, {})
        off = median_image(labels.get("all_off", []))
        all_on = median_image(labels.get("all_on", []))
        all_on_centroids = []
        if off is not None and all_on is not None:
            all_on_centroids = compact_bright_centroids(cv2.subtract(all_on, off), threshold)
        per_bit = []
        for bit in range(LED_MASK_BIT_COUNT):
            label = f"bit_{bit:02d}"
            frames = labels.get(label, [])
            image = median_image(frames)
            centroids = []
            if off is not None and image is not None:
                difference = cv2.subtract(image, off)
                centroids = compact_bright_centroids(difference, threshold)
            match_fraction = centroid_match_fraction(centroids, all_on_centroids)
            median_matching_component_count = round(len(centroids) * match_fraction)

            frame_matching_counts: list[int] = []
            if off is not None and all_on_centroids:
                for frame in frames:
                    frame_centroids = compact_bright_centroids(cv2.subtract(frame, off), threshold)
                    frame_matching_counts.append(matching_component_count(frame_centroids, all_on_centroids))

            # A single held bit producing both dark frames and frames containing
            # several of the same constellation points as all-on is direct
            # evidence of temporal modulation. A spatial selection bit cannot
            # change which emitters are enabled while its value is unchanged.
            lit_frame_count = sum(count >= 2 for count in frame_matching_counts)
            dark_frame_count = sum(count == 0 for count in frame_matching_counts)
            temporal_toggle = lit_frame_count > 0 and dark_frame_count > 0

            if median_matching_component_count > 0 or any(count > 0 for count in frame_matching_counts):
                observed_bits.add(bit)
            per_bit.append(
                {
                    "bit": bit,
                    "frame_count": len(frames),
                    "bright_component_count": len(centroids),
                    "centroids_px": centroids,
                    "fraction_matching_all_on_components": match_fraction,
                    "matching_all_on_component_count": median_matching_component_count,
                    "matching_all_on_component_counts_per_frame": frame_matching_counts,
                    "lit_frame_count": lit_frame_count,
                    "dark_frame_count": dark_frame_count,
                    "temporal_toggle_with_constant_bit": temporal_toggle,
                }
            )

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

    bits_with_multiple_matching_components = sorted(
        {
            entry["bit"]
            for camera in cameras
            for entry in camera["bits"]
            if entry["matching_all_on_component_count"] > 1
            or max(entry["matching_all_on_component_counts_per_frame"], default=0) > 1
        }
    )
    bits_with_single_matching_component = sorted(
        {
            entry["bit"]
            for camera in cameras
            for entry in camera["bits"]
            if entry["matching_all_on_component_count"] == 1
        }
    )
    temporal_waveform_evidence = [
        {
            "camera": camera["camera"],
            "bit": entry["bit"],
            "frame_count": entry["frame_count"],
            "lit_frame_count": entry["lit_frame_count"],
            "dark_frame_count": entry["dark_frame_count"],
            "matching_all_on_component_counts_per_frame": entry["matching_all_on_component_counts_per_frame"],
        }
        for camera in cameras
        for entry in camera["bits"]
        if entry["temporal_toggle_with_constant_bit"]
    ]
    bits_with_temporal_toggle = sorted({entry["bit"] for entry in temporal_waveform_evidence})
    unused_or_unseen_bits = sorted(set(range(LED_MASK_BIT_COUNT)) - observed_bits)
    semantics = classify_led_blink_semantics(
        observed_bits, bits_with_multiple_matching_components, bits_with_temporal_toggle
    )

    return {
        "format": "psvr2-sense-led-mask-analysis-v3",
        "capture_dir": str(capture_dir),
        "mask_manifest": str(manifest_path),
        "threshold": threshold,
        "segment_edge_margin_ms": margin_ms,
        "timing_alignment_basis": timing_basis,
        "candidate_mask_bit_count": LED_MASK_BIT_COUNT,
        "observed_bits": sorted(observed_bits),
        "unused_or_unseen_bits": unused_or_unseen_bits,
        "bits_with_single_matching_component": bits_with_single_matching_component,
        "bits_with_multiple_matching_components_in_one_camera": bits_with_multiple_matching_components,
        "bits_with_temporal_toggle": bits_with_temporal_toggle,
        "temporal_waveform_evidence": temporal_waveform_evidence,
        "led_blink_semantics_status": semantics,
        "cameras": cameras,
        "note": (
            "The strongest temporal-waveform evidence is a single unchanged led_blink bit producing both dark camera "
            "frames and lit frames containing several of the same spatial constellation points as the all-on reference. "
            "That cannot be explained by a static per-LED spatial mask. Multiple simultaneous all-on-matching points "
            "without an in-segment toggle support grouped/shared control but are weaker evidence on their own."
        ),
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
        f"Observed {len(result['observed_bits'])}/{LED_MASK_BIT_COUNT} candidate bits; "
        f"led_blink semantics={result['led_blink_semantics_status']}"
    )
    if result["bits_with_temporal_toggle"]:
        print(f"Constant-bit temporal toggles: {result['bits_with_temporal_toggle']}")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
