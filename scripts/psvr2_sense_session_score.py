#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Score a recorded PS Sense constellation session.

A session directory is produced by ``scripts/psvr2_sense_session.sh`` and contains:

    run.log      stderr of ``monado-cli psvr2-constellation`` (PSSENSE_TIMING_DIAG=1 recommended)
    poses.csv    stdout of the same command (10 Hz pose + diagnostics rows)
    capture/     optional stride-sampled camera frames with cameraN.csv manifests

Every live experiment is reduced to the same numbers, so changes can be compared run against run:

* illumination: LED bootstrap scan/lock timeline, per-step scores, locked lit fraction, and (when frames were
  captured) the per-camera fraction of frames showing at least N compact bright blobs;
* optical pipeline: candidates per camera, fused poses by camera count, reacquisitions, slow/fast sample drops,
  optical-vs-IMU orientation residuals;
* output: fraction of samples with a tracked position, pose age, and static jitter.

Usage:
    psvr2_sense_session_score.py SESSION_DIR [--json OUT] [--min-blobs 3] [--no-images]
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import re
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

POSITION_TRACKED_BIT = 1 << 5
BOOTSTRAP_STATES = {0: "idle", 1: "wide", 2: "narrow", 3: "locked", 4: "baseline"}

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
KV_RE = re.compile(r"(\w+)=(\([^)]*\)|\S+)")


# ---------------------------------------------------------------------------
# Log parsing
# ---------------------------------------------------------------------------


def parse_kv(text: str) -> dict[str, str]:
    return {k: v for k, v in KV_RE.findall(text)}


def to_float(value: str | None) -> float | None:
    if value is None:
        return None
    try:
        out = float(value)
    except ValueError:
        return None
    return out if math.isfinite(out) else None


def parse_log(lines) -> dict:
    """Extract the structured diagnostics the Sense driver and tracker print."""
    bootstrap: dict[str, list] = defaultdict(list)
    candidates: dict[str, Counter] = defaultdict(Counter)
    fused_cameras: dict[str, Counter] = defaultdict(Counter)
    reacquire: Counter = Counter()
    imu_aligned: dict[str, list] = defaultdict(list)
    clock: dict[str, list] = defaultdict(list)
    snaps: Counter = Counter()
    exposures: dict[str, list] = defaultdict(list)
    commands: dict[str, list] = defaultdict(list)  # (host_ns, phase) of each LED output report
    candidate_ts: dict[str, list] = defaultdict(list)
    counts = Counter()

    for raw in lines:
        line = ANSI_RE.sub("", raw).rstrip("\n")
        if "PSSENSE_TIMING" in line and "os_hid_iokit" in line:
            kv = parse_kv(line.split("PSSENSE_TIMING", 1)[1])
            host, phase = to_float(kv.get("host_now_ns")), kv.get("phase")
            if host is not None and phase is not None:
                # An empty blink mask (PSSENSE_LED_BOOTSTRAP_YIELD_MASK) is "off" too.
                if kv.get("masks") == "00000000":
                    phase = "5"
                commands[kv.get("side", "?")].append((host, phase))
        if "LED_BOOTSTRAP" in line:
            kv = parse_kv(line.split("LED_BOOTSTRAP", 1)[1])
            side = kv.get("side", "?")
            bootstrap[side].append(kv)
        elif "LED_SCHEDULE" in line:
            kv = parse_kv(line.split("LED_SCHEDULE", 1)[1])
            now, controller_now = to_float(kv.get("now")), to_float(kv.get("controller_now"))
            if now is not None and controller_now is not None and controller_now >= 0:
                clock[kv.get("side", "?")].append((now, controller_now - now))
            raw_exposure, period = to_float(kv.get("raw_exposure")), to_float(kv.get("period"))
            if raw_exposure and period:
                exposures[kv.get("side", "?")].append((raw_exposure, period))
        elif "CLOCK_OFFSET" in line and "event=snap" in line:
            snaps[parse_kv(line.split("CLOCK_OFFSET", 1)[1]).get("side", "?")] += 1
        elif "CONSTELLATION_CANDIDATE" in line:
            kv = parse_kv(line.split("CONSTELLATION_CANDIDATE", 1)[1])
            side = kv.get("side", "?")
            candidates[side][kv.get("cam", "?")] += 1
            ts = to_float(kv.get("ts"))
            if ts is not None:
                candidate_ts[side].append(ts)
            if kv.get("imu_aligned_valid") == "1":
                value = to_float(kv.get("imu_aligned_delta_deg"))
                if value is not None:
                    imu_aligned[side].append(value)
        elif "CONSTELLATION_FUSED_ACCEPT" in line:
            kv = parse_kv(line.split("CONSTELLATION_FUSED_ACCEPT", 1)[1])
            fused_cameras[kv.get("side", "?")][kv.get("cameras", "?")] += 1
        elif "CONSTELLATION_REACQUIRE" in line:
            kv = parse_kv(line.split("CONSTELLATION_REACQUIRE", 1)[1])
            reacquire[kv.get("side", "?")] += 1
        if "Dropping slow sample" in line:
            counts["dropped_slow_samples"] += 1
        if "Dropping fast sample" in line:
            counts["dropped_fast_samples"] += 1

    sides = sorted(set(bootstrap) | set(candidates) | set(fused_cameras) | set(reacquire) | set(clock))
    out = {"sides": {}, "counts": dict(counts)}
    for side in sides:
        out["sides"][side] = {
            "bootstrap": summarise_bootstrap(bootstrap.get(side, [])),
            "candidates_by_camera": dict(sorted(candidates[side].items())),
            "fused_by_camera_count": dict(sorted(fused_cameras[side].items())),
            "reacquisitions": reacquire[side],
            "imu_aligned_delta_deg": describe(imu_aligned[side]),
            "clock_offset": summarise_clock(clock.get(side, []), snaps[side]),
            "exposure_jitter_us": exposure_jitter_us(exposures.get(side, [])),
            "lit_while_off": lit_while_off(commands.get(side, []), candidate_ts.get(side, [])),
        }
    return out


def lit_while_off(commands: list[tuple[float, str]], candidates: list[float], settle_ms: float = 400.0) -> dict | None:
    """Pose candidates for a controller exposed while it had been commanded dark (LED_ALL_OFF, phase 5, or an empty
    blink mask) for at least
    settle_ms. A controller that obeys produces none; on 24 Sep the right Sense stayed lit through minutes of
    off commands, which poisoned every bootstrap baseline.
    """
    if not commands:
        return None
    commands = sorted(commands)
    off_since = []  # (host_ns, start of the current run of phase-5 commands or None)
    start = None
    for host, phase in commands:
        start = (start if start is not None else host) if phase == "5" else None
        off_since.append((host, start))
    hosts = [h for h, _ in off_since]


    hits = []
    for ts in candidates:
        i = bisect.bisect_right(hosts, ts) - 1
        if i < 0:
            continue
        start = off_since[i][1]
        if start is not None and ts - start >= settle_ms * 1e6:
            hits.append(ts)
    seconds = sorted({int((ts - commands[0][0]) / 1e9) for ts in hits})
    return {"candidates": len(hits), "seconds": seconds}


def exposure_jitter_us(samples: list[tuple[float, float]]) -> dict | None:
    """Residuals of the host-time exposure timestamps the LED schedule starts from, against the camera's grid.

    The camera runs on a fixed period, so anything here is host clock-mapping noise (hw2mono_vts), and it moves
    the scheduled LED pulse one for one.
    """
    unique = sorted({t for t, _ in samples})
    if len(unique) < 10:
        return None
    period = statistics.median(p for _, p in samples)
    k = [round((t - unique[0]) / period) for t in unique]
    mean_k, mean_t = statistics.fmean(k), statistics.fmean(unique)
    slope = sum((a - mean_k) * (b - mean_t) for a, b in zip(k, unique)) / sum((a - mean_k) ** 2 for a in k)
    residuals = [(t - mean_t - slope * (a - mean_k)) / 1000.0 for a, t in zip(k, unique)]
    return describe(residuals)


def summarise_clock(samples: list[tuple[float, float]], snaps: int, settle_us: float = 100.0) -> dict | None:
    """How far the scheduling clock offset (controller minus host) moved during the run, and when it settled.

    Every microsecond it creeps slides the scheduled LED pulse against the camera exposures by the same amount.
    """
    if not samples:
        return None
    t0, first = samples[0]
    offsets = [o for _, o in samples]
    final = statistics.median(offsets[-120:])
    outside = [i for i, o in enumerate(offsets) if abs(o - final) > settle_us * 1000.0]
    settled_s = 0.0 if not outside else (samples[min(outside[-1] + 1, len(samples) - 1)][0] - t0) / 1e9
    return {
        "creep_us": (final - first) / 1000.0,
        "range_us": (max(offsets) - min(offsets)) / 1000.0,
        "settled_s": settled_s,
        "snaps": snaps,
    }


def summarise_bootstrap(events: list[dict]) -> dict:
    summary = {
        "scans_started": 0,
        "scans_failed": [],
        "locks": [],
        "lost": 0,
        "locked_status": [],
        "last_scan_steps": {},
        "baselines": [],
    }
    steps: dict[str, list] = defaultdict(list)
    for kv in events:
        event = kv.get("event")
        if event == "scan_start":
            if kv.get("stage") == "wide":
                summary["scans_started"] += 1
                steps = defaultdict(list)
        elif event == "step":
            steps[kv.get("stage", "?")].append(
                {
                    "fudge_us": to_float(kv.get("fudge_us")),
                    "pulse_us": to_float(kv.get("pulse_us")),
                    "score": to_float(kv.get("score")),
                    "mean_blobs": to_float(kv.get("mean_blobs")),
                    "lit": kv.get("lit"),
                }
            )
        elif event == "baseline":
            summary["baselines"].append(kv.get("blobs"))
        elif event == "scan_failed":
            summary["scans_failed"].append(kv.get("reason"))
        elif event == "locked":
            summary["locks"].append({k: to_float(v) if k != "side" else v for k, v in kv.items() if k != "event"})
            summary["last_scan_steps"] = dict(steps)
        elif event == "lost":
            summary["lost"] += 1
        elif event == "locked_status":
            lit, _, total = (kv.get("lit_reports") or "0/0").partition("/")
            try:
                fraction = int(lit) / int(total) if int(total) else None
            except ValueError:
                fraction = None
            summary["locked_status"].append(fraction)
    if not summary["last_scan_steps"] and steps:
        summary["last_scan_steps"] = dict(steps)
    fractions = [f for f in summary["locked_status"] if f is not None]
    summary["locked_lit_fraction"] = describe(fractions)
    return summary


# ---------------------------------------------------------------------------
# Pose CSV
# ---------------------------------------------------------------------------


def describe(values: list[float]) -> dict | None:
    values = [v for v in values if v is not None and math.isfinite(v)]
    if not values:
        return None
    ordered = sorted(values)

    def pct(p):
        return ordered[min(len(ordered) - 1, int(round(p * (len(ordered) - 1))))]

    return {
        "n": len(ordered),
        "median": statistics.median(ordered),
        "p05": pct(0.05),
        "p95": pct(0.95),
        "max": ordered[-1],
    }


def parse_poses(path: Path) -> dict:
    rows_by_hand: dict[str, list[dict]] = defaultdict(list)
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            if not row.get("hand"):
                continue
            rows_by_hand[row["hand"]].append(row)

    out = {}
    for hand, rows in rows_by_hand.items():
        t0 = int(rows[0]["timestamp_ns"])
        tracked = []
        ages = []
        state_time = Counter()
        first_lock_s = None
        for row in rows:
            flags = int(row["relation_flags"], 0)
            t = (int(row["timestamp_ns"]) - t0) / 1e9
            is_tracked = bool(flags & POSITION_TRACKED_BIT)
            tracked.append((t, is_tracked, row))
            age = int(row["pose_age_ns"])
            if age >= 0:
                ages.append(age / 1e6)
            state = row.get("led_bootstrap_state")
            if state not in (None, ""):
                name = BOOTSTRAP_STATES.get(int(state), state)
                state_time[name] += 1
                if name == "locked" and first_lock_s is None:
                    first_lock_s = t

        out[hand] = {
            "samples": len(rows),
            "duration_s": (int(rows[-1]["timestamp_ns"]) - t0) / 1e9,
            "position_tracked_fraction": sum(1 for _, x, _ in tracked if x) / len(rows),
            "pose_age_ms": describe(ages),
            "static_jitter_mm": static_jitter_mm(tracked),
            "bootstrap_state_fraction": {k: v / len(rows) for k, v in sorted(state_time.items())},
            "first_lock_s": first_lock_s,
            "final": {k: rows[-1].get(k) for k in rows[-1] if k not in ("hand",)},
        }
    return out


def static_jitter_mm(tracked, window_s: float = 1.0, max_motion_mm: float = 20.0) -> dict | None:
    """RMS deviation from the window mean, over 1 s windows whose extent suggests the controller was still."""
    windows = defaultdict(list)
    for t, is_tracked, row in tracked:
        if is_tracked:
            windows[int(t // window_s)].append(tuple(float(row[k]) * 1000.0 for k in ("px", "py", "pz")))
    values = []
    for points in windows.values():
        if len(points) < 5:
            continue
        mean = [sum(p[i] for p in points) / len(points) for i in range(3)]
        extent = max(math.dist(p, mean) for p in points)
        if extent > max_motion_mm:
            continue
        values.append(math.sqrt(sum(math.dist(p, mean) ** 2 for p in points) / len(points)))
    return describe(values)


# ---------------------------------------------------------------------------
# Captured frames
# ---------------------------------------------------------------------------


def read_pgm(path: Path):
    import numpy as np

    data = path.read_bytes()
    parts = []
    index = 0
    while len(parts) < 4:
        while data[index : index + 1].isspace():
            index += 1
        if data[index : index + 1] == b"#":
            index = data.index(b"\n", index) + 1
            continue
        end = index
        while not data[end : end + 1].isspace():
            end += 1
        parts.append(data[index:end])
        index = end
    index += 1
    width, height = int(parts[1]), int(parts[2])
    return np.frombuffer(data, dtype=np.uint8, count=width * height, offset=index).reshape(height, width)


def count_compact_blobs(image, threshold=80, peak=180, max_size=50, active_width=508) -> int:
    import cv2

    image = image[:, :active_width]
    _, mask = cv2.threshold(image, threshold - 1, 255, cv2.THRESH_BINARY)
    n, labels, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
    count = 0
    for i in range(1, n):
        w, h = stats[i, cv2.CC_STAT_WIDTH], stats[i, cv2.CC_STAT_HEIGHT]
        if w > max_size or h > max_size:
            continue
        if image[labels == i].max() >= peak:
            count += 1
    return count


def analyse_capture(capture: Path, poses_path: Path | None, min_blobs: int) -> dict | None:
    manifests = sorted(capture.glob("camera*.csv"))
    if not manifests:
        return None
    try:
        import cv2  # noqa: F401
        import numpy  # noqa: F401
    except ImportError:
        return {"skipped": "numpy/opencv not available"}

    # Label frames by the left controller's bootstrap state at the nearest earlier pose row (same monotonic clock).
    timeline = []
    if poses_path and poses_path.exists():
        with poses_path.open(newline="") as f:
            for row in csv.DictReader(f):
                if row.get("led_bootstrap_state") not in (None, ""):
                    timeline.append((int(row["timestamp_ns"]), BOOTSTRAP_STATES.get(int(row["led_bootstrap_state"]))))
        timeline.sort()

    def state_at(ts):
        lo, hi = 0, len(timeline)
        while lo < hi:
            mid = (lo + hi) // 2
            if timeline[mid][0] <= ts:
                lo = mid + 1
            else:
                hi = mid
        return timeline[lo - 1][1] if lo > 0 else None

    out = {}
    for manifest in manifests:
        per_state = defaultdict(lambda: [0, 0])
        with manifest.open(newline="") as f:
            for row in csv.DictReader(f):
                frame = capture / row["file"]
                if not frame.exists():
                    continue
                blobs = count_compact_blobs(read_pgm(frame))
                state = state_at(int(row["exposure_monotonic_ns"])) if timeline else "all"
                bucket = per_state[state or "unknown"]
                bucket[0] += 1
                bucket[1] += blobs >= min_blobs
        out[manifest.stem] = {
            state: {"frames": n, "lit_frames": lit, "lit_fraction": lit / n if n else None}
            for state, (n, lit) in sorted(per_state.items())
        }
    return out


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------


def fmt(value, digits=2):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def score_bar(score: float | None, max_score: float = 4.0, width: int = 24) -> str:
    if score is None:
        return ""
    return "#" * int(round(width * min(score, max_score) / max_score))


def render_text(result: dict) -> str:
    lines = [f"Session: {result['session']}"]
    for hand, p in result.get("poses", {}).items():
        lines.append(
            f"[{hand}] {p['samples']} samples over {fmt(p['duration_s'], 1)} s; position tracked "
            f"{fmt(100 * p['position_tracked_fraction'], 1)}%; first LED lock at {fmt(p['first_lock_s'], 1)} s"
        )
        if p["pose_age_ms"]:
            lines.append(f"    pose age ms: median {fmt(p['pose_age_ms']['median'], 1)}, p95 {fmt(p['pose_age_ms']['p95'], 1)}")
        if p["static_jitter_mm"]:
            lines.append(
                f"    static jitter mm (1 s windows): median {fmt(p['static_jitter_mm']['median'])}, "
                f"p95 {fmt(p['static_jitter_mm']['p95'])} over {p['static_jitter_mm']['n']} windows"
            )
        if p["bootstrap_state_fraction"]:
            states = ", ".join(f"{k} {fmt(100 * v, 0)}%" for k, v in p["bootstrap_state_fraction"].items())
            lines.append(f"    bootstrap state time: {states}")

    for side, s in result.get("log", {}).get("sides", {}).items():
        b = s["bootstrap"]
        lines.append(f"[{side}] log")
        lines.append(
            f"    bootstrap: {b['scans_started']} scans, {len(b['locks'])} locks, {b['lost']} lost, "
            f"failures {Counter(b['scans_failed']) or '-'}"
        )
        for lock in b["locks"]:
            lines.append(
                f"    lock: lit window {fmt(lock.get('lit_start_us'), 0)}-{fmt(lock.get('lit_end_us'), 0)} us "
                f"({fmt(lock.get('window_us'), 0)} us), centre {fmt(lock.get('centre_us'), 0)} us, "
                f"fudge {fmt(lock.get('lock_fudge_us'), 0)} us, pulse {fmt(lock.get('lock_pulse_us'), 0)} us"
            )
        if b.get("baselines"):
            lines.append(f"    dark baseline blobs per camera: {', '.join(b['baselines'])}")
        if b["locked_lit_fraction"]:
            lines.append(f"    locked lit fraction: median {fmt(b['locked_lit_fraction']['median'])}")
        for stage, steps in b["last_scan_steps"].items():
            lines.append(f"    {stage} scan (fudge us: score)")
            for step in steps:
                lines.append(
                    f"      {fmt(step['fudge_us'], 0):>7}: {fmt(step['score']):>5} {score_bar(step['score'])}"
                )
        lines.append(f"    candidates by camera: {s['candidates_by_camera'] or '-'}")
        lines.append(f"    fused poses by camera count: {s['fused_by_camera_count'] or '-'}")
        lines.append(f"    reacquisitions: {s['reacquisitions']}")
        c = s.get("clock_offset")
        if c:
            lines.append(
                f"    clock offset: creep {fmt(c['creep_us'], 1)} us (range {fmt(c['range_us'], 1)} us), "
                f"settled within 100 us at {fmt(c['settled_s'], 1)} s, snaps {c['snaps']}"
            )
        w = s.get("lit_while_off")
        if w:
            flag = "  <-- controller ignored LED_ALL_OFF" if w["candidates"] > 20 else ""
            lines.append(
                f"    candidates while commanded off > 400 ms: {w['candidates']} in {len(w['seconds'])} s{flag}"
            )
        j = s.get("exposure_jitter_us")
        if j:
            lines.append(
                f"    exposure timestamp residual us: p5 {fmt(j['p05'], 0)}, median {fmt(j['median'], 0)}, "
                f"p95 {fmt(j['p95'], 0)}"
            )
        if s["imu_aligned_delta_deg"]:
            lines.append(f"    optical vs aligned IMU deg: median {fmt(s['imu_aligned_delta_deg']['median'])}")
    counts = result.get("log", {}).get("counts", {})
    if counts:
        lines.append(f"tracker: {counts}")

    capture = result.get("capture")
    if capture:
        lines.append(f"captured frames with >= {result['min_blobs']} compact bright blobs:")
        if "skipped" in capture:
            lines.append(f"    skipped: {capture['skipped']}")
        else:
            for camera, states in capture.items():
                parts = ", ".join(
                    f"{state} {st['lit_frames']}/{st['frames']}" for state, st in states.items()
                )
                lines.append(f"    {camera}: {parts}")
    return "\n".join(lines)


def score_session(session: Path, min_blobs: int = 3, images: bool = True) -> dict:
    result: dict = {"session": str(session), "min_blobs": min_blobs}
    log = session / "run.log"
    poses = session / "poses.csv"
    if log.exists():
        with log.open(errors="replace") as f:
            result["log"] = parse_log(f)
    if poses.exists():
        result["poses"] = parse_poses(poses)
    capture = session / "capture"
    if images and capture.is_dir():
        result["capture"] = analyse_capture(capture, poses if poses.exists() else None, min_blobs)
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("session", type=Path)
    parser.add_argument("--json", type=Path, help="write the full result as JSON")
    parser.add_argument("--min-blobs", type=int, default=3)
    parser.add_argument("--no-images", action="store_true", help="skip captured-frame analysis")
    args = parser.parse_args(argv)

    if not args.session.is_dir():
        print(f"not a directory: {args.session}", file=sys.stderr)
        return 1
    result = score_session(args.session, args.min_blobs, not args.no_images)
    print(render_text(result))
    if args.json:
        args.json.write_text(json.dumps(result, indent=2, default=str) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
