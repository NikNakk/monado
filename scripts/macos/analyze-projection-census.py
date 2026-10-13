#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Summarize complete native composition traces; keep raw captures outside Git."""
import argparse
import collections
import csv
import json
from pathlib import Path

REASONS = {1: "layer_count", 2: "passthrough", 4: "peek", 8: "debug_mirror",
           16: "debug_disabled", 32: "layer_type", 64: "color_scale_bias", 128: "chroma_key"}
TYPES = {0: "projection", 1: "projection_depth", 2: "quad", 3: "cube", 4: "cylinder",
         5: "equirect1", 6: "equirect2", 7: "passthrough"}


def summarize(path, trim_seconds):
    with path.open(newline="") as file:
        rows = list(csv.DictReader(file))
    if not rows:
        raise ValueError(f"Empty census: {path}")
    groups = collections.defaultdict(list)
    for row in rows:
        groups[row["compositor_id"], int(row["event_ns"]), int(row["frame_id"])].append(row)
    complete = []
    for key, group in groups.items():
        summaries = [r for r in group if int(r["layer_index"]) == -1]
        if len(summaries) != 1:
            raise ValueError(f"Missing/duplicate frame summary: {path} {key}")
        frame = summaries[0]
        layers = sorted((r for r in group if int(r["layer_index"]) >= 0), key=lambda r: int(r["layer_index"]))
        count = int(frame["layer_count"])
        if [int(r["layer_index"]) for r in layers] != list(range(count)):
            raise ValueError(f"Incomplete layer list: {path} {key}")
        if any(r["rejection_mask"] != frame["rejection_mask"] or r["fast_path"] != frame["fast_path"] for r in layers):
            raise ValueError(f"Inconsistent frame metadata: {path} {key}")
        expected = sum(bit for bit, name in ((2, "passthrough"), (4, "peek"), (8, "debug_mirror"), (16, "debug_disable"))
                       if int(frame[name]))
        if count != 1:
            expected |= 1
        else:
            layer = layers[0]
            kind = int(layer["layer_type"])
            if kind not in (0, 1): expected |= 32
            if int(layer["flags"]) & 16: expected |= 64
            if kind in (0, 1) and float(layer["chroma_curve"]) > 0: expected |= 128
        if int(frame["rejection_mask"]) != expected or bool(int(frame["fast_path"])) != (expected == 0):
            raise ValueError(f"Selection does not match policy: {path} {key}")
        complete.append((frame, layers))
    # Trim each compositor instance separately rather than joining unrelated lifetimes.
    windows = {}
    for instance in {r["compositor_id"] for r, _ in complete}:
        times = [int(r["event_ns"]) for r, _ in complete if r["compositor_id"] == instance]
        windows[instance] = (min(times) + int(trim_seconds * 1e9), max(times) - int(trim_seconds * 1e9))
    steady = [(r, layers) for r, layers in complete
              if windows[r["compositor_id"]][0] <= int(r["event_ns"]) <= windows[r["compositor_id"]][1]]
    patterns, reasons, flags, sizes = (collections.Counter() for _ in range(4))
    identity_scale_bias = 0
    for frame, layers in steady:
        pattern = "+".join(TYPES.get(int(r["layer_type"]), "unknown") for r in layers) or "empty"
        patterns[pattern] += 1
        mask = int(frame["rejection_mask"])
        reasons["|".join(name for bit, name in REASONS.items() if mask & bit) or "eligible"] += 1
        identity = False
        for layer in layers:
            name = TYPES.get(int(layer["layer_type"]), "unknown")
            flags[f"{name}:0x{int(layer['flags']):x}"] += 1
            if int(layer["view_count"]):
                sizes[f"{name}:{layer['source_width']}x{layer['source_height']}"] += 1
            if int(layer["flags"]) & 16:
                identity |= all(float(layer[f"color_scale_{axis}"]) == 1 and float(layer[f"color_bias_{axis}"]) == 0
                                for axis in "rgba")
        identity_scale_bias += identity
    return {"trace": str(path), "total_complete_frames": len(complete), "steady_frames": len(steady),
            "status": "measured" if steady else "insufficient_duration",
            "nonempty_frames": sum(bool(layers) for _, layers in steady),
            "trim_seconds": trim_seconds, "fast_path_frames": sum(int(r["fast_path"]) for r, _ in steady),
            "compute_frames": sum(int(r["use_compute"]) for r, _ in steady), "layer_patterns": dict(patterns),
            "rejections": dict(reasons), "layer_flags": dict(flags), "projection_source_sizes": dict(sizes),
            "frames_with_identity_color_scale_bias_flag": identity_scale_bias}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path, help="Composition CSVs or capture directories")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--trim-seconds", type=float, default=2)
    args = parser.parse_args()
    if args.trim_seconds < 0: parser.error("Trim must be nonnegative")
    paths = []
    for path in args.captures:
        paths.extend(sorted(path.rglob("*_composition.csv")) if path.is_dir() else [path])
    if not paths: parser.error("No composition traces found")
    result = [summarize(path, args.trim_seconds) for path in paths]
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output: args.output.write_text(rendered)
    print(rendered, end="")


if __name__ == "__main__":
    main()
