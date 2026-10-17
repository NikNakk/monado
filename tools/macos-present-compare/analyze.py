#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Summarize buffered presentation traces; never substitute missing feedback."""
import argparse
import collections
import csv
import json
import math
import pathlib
import statistics


def percentile(values, q):
    values = sorted(values)
    if not values:
        return None
    index = (len(values) - 1) * q
    lo = math.floor(index)
    hi = math.ceil(index)
    return values[lo] + (values[hi] - values[lo]) * (index - lo)


def distribution(values):
    return {
        "count": len(values),
        "p50": percentile(values, .5),
        "p95": percentile(values, .95),
        "p99": percentile(values, .99),
        "max": max(values) if values else None,
    }


def summarize(path, warmup, refresh_hz, trust_wsi_clock):
    metadata = {}
    for line in pathlib.Path(str(path) + ".meta.txt").read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            metadata[key] = value
    if metadata.get("result") != "complete":
        raise ValueError(f"{path}: incomplete/failed capture")
    with path.open(newline="") as stream:
        all_rows = list(csv.DictReader(stream))
    rows = [r for r in all_rows if int(r["frame_id"]) > warmup]
    if not rows:
        raise ValueError(f"{path}: no frames after warm-up")
    hz = refresh_hz or float(metadata.get("nominal_refresh_hz", 0))
    if not math.isfinite(hz) or hz <= 0:
        raise ValueError(f"{path}: supply --refresh-hz (no valid display period)")
    period = 1 / hz
    # Unknown feedback can mean an unpresented frame OR a missing callback.
    presented = sorted((r for r in rows if r["status"] == "presented" and float(r["presented_s"]) > 0),
                       key=lambda r: float(r["presented_s"]))
    times = [float(r["presented_s"]) for r in presented]
    intervals = [b - a for a, b in zip(times, times[1:])]
    missed = [max(0, round(dt / period) - 1) for dt in intervals]
    ticks = [int(r["tick_id"]) for r in rows]
    clock_comparable = metadata["mode"] == "native" or trust_wsi_clock
    plausible = all(float(r["generated_s"]) - .005 <= float(r["presented_s"]) <=
                    float(r["feedback_received_s"]) + .005 for r in presented)
    clock_comparable = clock_comparable and plausible
    result = {
        "file": str(path), "mode": metadata["mode"], "policy": metadata["policy"],
        "display": metadata.get("display"), "size": [int(metadata["width"]), int(metadata["height"])],
        "burn": int(metadata["burn"]), "refresh_hz": hz, "warmup_frames": warmup,
        "rows": len(rows), "statuses": dict(collections.Counter(r["status"] for r in rows)),
        "feedback_coverage_percent": len(presented) / len(rows) * 100,
        "coalesced_display_ticks": sum(max(0, b - a - 1) for a, b in zip(ticks, ticks[1:])),
        "observed_interval_ms": distribution([x * 1000 for x in intervals]),
        "observed_long_intervals": sum(x > 0 for x in missed),
        "estimated_unobserved_refreshes": sum(missed),
        "observed_hz": (len(times) - 1) / (times[-1] - times[0]) if len(times) > 1 and times[-1] > times[0] else None,
        "host_clock_plausible": plausible, "host_age_enabled": clock_comparable,
        "acquire_wait_ms": distribution([(float(r["acquire_end_s"]) - float(r["acquire_begin_s"])) * 1000
                                          for r in rows if float(r["acquire_end_s"]) > 0]),
        "reuse_wait_ms": distribution([float(r["reuse_wait_ms"]) for r in rows]),
        "submit_call_ms": distribution([(float(r["submit_end_s"]) - float(r["submit_begin_s"])) * 1000 for r in rows]),
        "present_call_ms": distribution([(float(r["present_end_s"]) - float(r["present_begin_s"])) * 1000
                                         for r in rows if float(r["present_end_s"]) > 0]),
        "render_gpu_ms": distribution([float(r["render_gpu_ms"]) for r in rows]),
        "post_render_gpu_ms": distribution([float(r["post_render_gpu_ms"]) for r in rows]),
    }
    if metadata["mode"] == "native":
        result["metal_command_gpu_ms"] = distribution([float(r["metal_command_gpu_ms"]) for r in rows
                                                       if float(r["present_end_s"]) > 0])
    if clock_comparable:
        result["frame_age_ms"] = distribution([(float(r["presented_s"]) - float(r["generated_s"])) * 1000 for r in presented])
        result["target_error_ms"] = distribution([(float(r["presented_s"]) - float(r["target_s"])) * 1000 for r in presented])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", type=pathlib.Path)
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--refresh-hz", type=float)
    parser.add_argument("--trust-wsi-clock", action="store_true",
                        help="enable host-age comparisons after verifying installed MoltenVK's GOOGLE clock")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error("warm-up must be nonnegative")
    results = [summarize(p, args.warmup, args.refresh_hz, args.trust_wsi_clock) for p in args.traces]
    if args.json:
        print(json.dumps(results, indent=2, allow_nan=False))
        return
    for r in results:
        print(f"{r['file']}: {r['mode']}/{r['policy']}, {r['size']}, burn={r['burn']}")
        print(f"  feedback={r['feedback_coverage_percent']:.2f}% statuses={r['statuses']}; coalesced ticks={r['coalesced_display_ticks']}")
        print(f"  observed Hz={r['observed_hz']}; long observed intervals={r['observed_long_intervals']}; estimated unobserved refreshes={r['estimated_unobserved_refreshes']}")
        for key in ("observed_interval_ms", "frame_age_ms", "target_error_ms", "acquire_wait_ms", "reuse_wait_ms",
                    "submit_call_ms", "present_call_ms", "render_gpu_ms", "post_render_gpu_ms", "metal_command_gpu_ms"):
            if key not in r:
                continue
            d = r[key]
            if d["count"]:
                print(f"  {key}: p50={d['p50']:.3f} p95={d['p95']:.3f} p99={d['p99']:.3f} max={d['max']:.3f}")
        if not r["host_age_enabled"]:
            print("  host frame age omitted: unverified or implausible WSI clock")
        if r["feedback_coverage_percent"] < 100:
            print("  Missing feedback is not counted as a confirmed dropped frame; interval gaps may reflect missing feedback.")


if __name__ == "__main__":
    main()
