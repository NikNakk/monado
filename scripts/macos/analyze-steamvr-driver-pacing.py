#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Join the SteamVR bridge driver's frame trace with Monado's app_pacing trace.

The driver trace (MWXR_DRIVER_FRAME_TRACE=1, driver-frames.csv) is in QPC
microseconds; its header gives the XrTime offset. Monado's app_pacing CSV is in
the service's monotonic nanoseconds, which differ from XrTime by a constant.
Monado's XrTime is its monotonic clock minus a per-instance constant, so frames
are aligned by matching each frame's predicted display time exactly; the
matching difference converts Monado's times to the driver's clock.
"""
import argparse
import collections
import csv
import json
import re
import statistics
from pathlib import Path


def read_driver(text):
    offset, frames, gpu, vsync = None, {}, {}, []
    for line in text.splitlines():
        if line.startswith("#"):
            m = re.search(r"xr_time_ns=qpc_us\*1000([+-]\d+)", line)
            offset = int(m.group(1)) if m else offset
            continue
        f = line.split(",")
        if f[0] == "frame" and len(f) == 16:
            keys = ["wait_begin", "wait_end", "predicted_display", "period_us", "should_render", "begin_end",
                    "present_begin", "prepared", "acquire_end", "acquired", "released", "end_begin", "end_end",
                    "layers"]
            frames[int(f[1])] = dict(zip(keys, map(int, f[2:])))
        elif f[0] == "gpu" and len(f) == 5 and f[4] == "1":
            gpu[(int(f[1]), f[2])] = int(f[3])
        elif f[0] == "vsync" and len(f) == 4:
            vsync.append(int(f[1]))
    if offset is None:
        raise ValueError("Driver trace has no XrTime offset header")
    return offset, frames, gpu, vsync


def read_app_pacing(text):
    rows = collections.defaultdict(dict)
    for r in csv.DictReader(text.splitlines()):
        rows[r["event"]][int(r["client_frame_id"])] = r
    return rows


def align(frames, offset, predicts, max_shift=40):
    """Returns (shift, clock difference ns): Monado id = driver frame + shift.

    Monado's XrTime is its monotonic clock minus a per-instance constant, so
    the same frame's predicted display differs between the traces by exactly
    that constant. A wrong shift matches only where neighbouring frame
    intervals happen to be equal, so the shift with the most exact matches
    wins, and it must match nearly every frame.
    """
    scores = []
    for shift in range(-5, max_shift):
        diffs = [int(predicts[n + shift]["predicted_display_ns"]) - (f["predicted_display"] * 1000 + offset)
                 for n, f in frames.items() if n + shift in predicts and f["predicted_display"]]
        if len(diffs) < max(3, len(frames) * 4 // 5):
            continue
        clock = statistics.median(diffs)
        scores.append((sum(abs(d - clock) <= 2_000 for d in diffs) / len(diffs), shift, clock))
    good = [s for s in scores if s[0] >= 0.95]
    if len(good) != 1:
        raise ValueError("Could not align driver frames with app_pacing predictions" +
                         (" (several shifts fit: cadence too regular)" if good else ""))
    return good[0][1], good[0][2]


def summary(values):
    v = sorted(values)
    if not v:
        return {"n": 0}
    q = lambda p: round(v[min(len(v) - 1, int(p * len(v)))], 3)
    return {"n": len(v), "p10": q(0.1), "p50": q(0.5), "p90": q(0.9), "max": round(v[-1], 3)}


def analyze(driver_text, pacing_text, skip_seconds=0.0):
    offset, frames, gpu, vsync = read_driver(driver_text)
    pacing = read_app_pacing(pacing_text)
    shift, clock_ns = align(frames, offset, pacing["predict"])
    first = min(f["wait_begin"] for f in frames.values())
    # xrWaitFrame reports the pacer's period; the shortest is the refresh.
    refresh_us = min((f["period_us"] for f in frames.values() if f["period_us"]), default=1)
    m = collections.defaultdict(list)
    multiples = collections.Counter()
    ms = lambda a, b: (a - b) / 1000.0
    for n, f in sorted(frames.items()):
        if f["wait_begin"] - first < skip_seconds * 1e6:
            continue
        m["wait"].append(ms(f["wait_end"], f["wait_begin"]))
        m["lead_after_wait"].append(ms(f["predicted_display"], f["wait_end"]))
        if f["present_begin"]:
            m["steamvr_frame"].append(ms(f["present_begin"], f["begin_end"]))
            m["prepare"].append(ms(f["prepared"], f["present_begin"]))
            m["acquire"].append(ms(f["acquire_end"], f["prepared"]))
            m["copy_and_release_cpu"].append(ms(f["released"], f["acquire_end"]))
            m["end_frame"].append(ms(f["end_end"], f["end_begin"]))
            m["driver_frame_begin_to_end"].append(ms(f["end_end"], f["begin_end"]))
        if (n, "source") in gpu and f["acquired"]:
            m["gpu_source_after_acquire"].append(ms(gpu[(n, "source")], f["acquire_end"]))
        if (n, "copies") in gpu and (n, "source") in gpu:
            m["gpu_copies_after_source"].append(ms(gpu[(n, "copies")], gpu[(n, "source")]))
        if (n, "copies") in gpu and f["end_end"]:
            m["gpu_copies_after_end_frame"].append(ms(gpu[(n, "copies")], f["end_end"]))
        if f["period_us"]:
            multiples[round(f["period_us"] / refresh_us)] += 1
        d = pacing["gpu_done"].get(n + shift)
        if d:
            m["monado_draw"].append(int(d["draw_actual_ns"]) / 1e6)
            m["monado_gpu"].append(int(d["gpu_actual_ns"]) / 1e6)
            if (n, "copies") in gpu:
                done_qpc_us = (int(d["event_ns"]) - clock_ns - offset) / 1000.0
                m["monado_gpu_done_after_driver_copies"].append((done_qpc_us - gpu[(n, "copies")]) / 1000.0)
    gaps = [ms(b, a) for a, b in zip(vsync, vsync[1:])]
    return {
        "limitation": "One session; GPU rows include listener latency; stages are CPU wall time.",
        "alignment": {"monado_frame_id_minus_driver_frame": shift, "monado_minus_xrtime_ns": clock_ns},
        "pacer_period_in_refreshes": dict(sorted(multiples.items())),
        "vsync_interval_ms": summary(gaps),
        "stages_ms": {k: summary(v) for k, v in m.items()},
    }


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("driver_frames", type=Path, help="driver-frames.csv from the run's log folder")
    p.add_argument("app_pacing", type=Path, help="Monado's monado_psvr2_<pid>_app_pacing.csv")
    p.add_argument("--skip-seconds", type=float, default=60.0, help="ignore start-up (default 60)")
    p.add_argument("--output", type=Path)
    a = p.parse_args()
    result = analyze(a.driver_frames.read_text(errors="replace"), a.app_pacing.read_text(errors="replace"),
                     a.skip_seconds)
    rendered = json.dumps(result, indent=2) + "\n"
    if a.output:
        a.output.write_text(rendered)
    print(rendered, end="")


if __name__ == "__main__":
    main()
