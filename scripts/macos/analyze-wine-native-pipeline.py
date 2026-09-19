#!/usr/bin/env python3
"""Join Wine/OpenComposite client frames to native macOS compositor/presentation traces."""

from __future__ import annotations

import csv
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def load(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def i(row: dict[str, str], key: str, default: int = 0) -> int:
    try:
        return int(row.get(key, "") or default)
    except ValueError:
        return default


def f(row: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(row.get(key, "") or default)
    except ValueError:
        return default


def percentile(values: list[float], p: float) -> float:
    if not values:
        return math.nan
    v = sorted(values)
    if len(v) == 1:
        return v[0]
    pos = (len(v) - 1) * p
    lo, hi = math.floor(pos), math.ceil(pos)
    if lo == hi:
        return v[lo]
    frac = pos - lo
    return v[lo] * (1 - frac) + v[hi] * frac


def describe(name: str, values: list[float], unit: str = "us") -> None:
    if not values:
        print(f"{name:30s} no data")
        return
    print(
        f"{name:30s} "
        f"p50={percentile(values, .50):9.1f} {unit}  "
        f"p95={percentile(values, .95):9.1f} {unit}  "
        f"p99={percentile(values, .99):9.1f} {unit}  "
        f"max={max(values):9.1f} {unit}  "
        f"mean={statistics.fmean(values):9.1f} {unit}"
    )


def event_index(rows: list[dict[str, str]]) -> dict[int, dict[str, dict[str, str]]]:
    out: dict[int, dict[str, dict[str, str]]] = defaultdict(dict)
    for row in rows:
        frame = i(row, "client_frame_id", -1)
        if frame >= 0:
            out[frame][row.get("event", "")] = row
    return out


def newest(directory: Path, pattern: str) -> Path | None:
    matches = list(directory.glob(pattern))
    return max(matches, key=lambda p: p.stat().st_mtime) if matches else None


def main() -> int:
    directory = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    if not directory.is_dir():
        print(f"Not a directory: {directory}", file=sys.stderr)
        return 2

    submit_path = newest(directory, "monado_psvr2_*_wine_submit.csv")
    if submit_path is None:
        print("No monado_psvr2_*_wine_submit.csv found.", file=sys.stderr)
        return 1

    m = re.search(r"monado_psvr2_(\d+)_wine_submit\.csv$", submit_path.name)
    if not m:
        print(f"Could not determine native service PID from {submit_path.name}", file=sys.stderr)
        return 1
    pid = m.group(1)

    def companion(suffix: str) -> Path:
        return directory / f"monado_psvr2_{pid}_{suffix}.csv"

    gpu_path = companion("client_gpu")
    map_path = companion("client_frame_map")
    pipeline_path = companion("frame_pipeline")
    present_path = companion("present")
    presented_path = companion("presented")
    complete_path = companion("present_complete")
    swapchain_path = companion("wine_swapchain")
    pacing_path = companion("app_pacing")
    wine_path = directory / "wine.csv"

    submit = load(submit_path)
    gpu = load(gpu_path)
    fmap = load(map_path)
    pipeline = load(pipeline_path)
    present = load(present_path)
    presented = load(presented_path)
    complete = load(complete_path)
    swapchain = load(swapchain_path)
    pacing = load(pacing_path)
    wine = load(wine_path)

    print(f"Trace directory: {directory}")
    print(f"Native service PID: {pid}")
    for name, path, rows in [
        ("Wine client", wine_path, wine),
        ("Wine native submit", submit_path, submit),
        ("Client GPU", gpu_path, gpu),
        ("Client/system map", map_path, fmap),
        ("System pipeline", pipeline_path, pipeline),
        ("Present", present_path, present),
        ("Presented", presented_path, presented),
        ("Present complete", complete_path, complete),
        ("Wine swapchain", swapchain_path, swapchain),
        ("App pacing", pacing_path, pacing),
    ]:
        print(f"  {name:20s} {len(rows):6d} rows  {path.name}{'' if rows else ' [missing/empty]'}")
    print()

    if wine:
        print("Wine client timing")
        for key, label in [
            ("wait_frame_us", "xrWaitFrame"),
            ("producer_wait_us", "DXMT signal/flush"),
            ("ipc_commit_us", "IPC submit"),
            ("layer_commit_total_us", "layer submit total"),
        ]:
            describe(label, [f(row, key) for row in wine])
        gpu_count = sum(1 for row in wine if i(row, "gpu_sync") != 0)
        print(f"GPU-sync Wine frames: {gpu_count}/{len(wine)}")
        print()

    if swapchain:
        waits = [i(row, "duration_ns") / 1000.0 for row in swapchain if row.get("event") == "wait"]
        acquires = [i(row, "duration_ns") / 1000.0 for row in swapchain if row.get("event") == "acquire"]
        releases = [i(row, "duration_ns") / 1000.0 for row in swapchain if row.get("event") == "release"]
        print("OpenComposite OpenXR swapchain operations")
        describe("xrAcquireSwapchainImage native", acquires)
        describe("xrWaitSwapchainImage native", waits)
        describe("xrReleaseSwapchainImage native", releases)
        for threshold_ms in (1, 4, 8, 16, 32):
            if waits:
                threshold_us = threshold_ms * 1000.0
                count = sum(v >= threshold_us for v in waits)
                print(f"swapchain waits >= {threshold_ms:2d} ms: {count:5d} ({100*count/len(waits):6.2f}%)")
        by_sc: dict[int, list[float]] = defaultdict(list)
        for row in swapchain:
            if row.get("event") == "wait":
                by_sc[i(row, "swapchain_id")].append(i(row, "duration_ns") / 1000.0)
        for sc, vals in sorted(by_sc.items()):
            print(f"  swapchain {sc}: waits={len(vals)} p50={percentile(vals,.50):.1f}us p95={percentile(vals,.95):.1f}us max={max(vals):.1f}us")
        print()

    if pacing:
        pred = [row for row in pacing if row.get("event") == "predict"]
        delivered = [row for row in pacing if row.get("event") == "delivered"]
        gpu_done = [row for row in pacing if row.get("event") == "gpu_done"]

        period_us = [i(row, "predicted_period_ns") / 1000.0 for row in pred]
        predict_lead_us = [
            (i(row, "predicted_display_ns") - i(row, "event_ns")) / 1000.0
            for row in pred
            if i(row, "predicted_display_ns") and i(row, "event_ns")
        ]
        submit_lead_us = [
            (i(row, "display_time_ns") - i(row, "event_ns")) / 1000.0
            for row in delivered
            if i(row, "display_time_ns") and i(row, "event_ns")
        ]
        draw_actual_us = [i(row, "draw_actual_ns") / 1000.0 for row in gpu_done]
        gpu_actual_us = [i(row, "gpu_actual_ns") / 1000.0 for row in gpu_done]
        draw_est_us = [i(row, "draw_est_ns") / 1000.0 for row in pred]
        gpu_est_us = [i(row, "gpu_est_ns") / 1000.0 for row in pred]
        cpu_est_us = [i(row, "cpu_est_ns") / 1000.0 for row in pred]

        print("Monado client pacing")
        describe("predicted app period", period_us)
        describe("predicted display lead", predict_lead_us)
        describe("display lead at delivery", submit_lead_us)
        describe("CPU estimate", cpu_est_us)
        describe("draw estimate", draw_est_us)
        describe("GPU estimate", gpu_est_us)
        describe("actual Begin->End draw", draw_actual_us)
        describe("actual delivered->GPU", gpu_actual_us)

        if period_us:
            refresh_us = 1_000_000.0 / 120.0
            rounded_divisors: dict[int, int] = defaultdict(int)
            for value in period_us:
                rounded_divisors[max(1, int(round(value / refresh_us)))] += 1
            print("Predicted-period refresh divisors:")
            print("  " + ", ".join(f"{k}x={v}" for k, v in sorted(rounded_divisors.items())))
        print()

    submit_ev = event_index(submit)
    gpu_ev = event_index(gpu)
    frames = sorted(set(submit_ev) | set(gpu_ev))

    handler_us: list[float] = []
    submit_to_ready_us: list[float] = []
    semaphore_wait_us: list[float] = []
    scheduled_wait_us: list[float] = []
    submit_to_scheduled_us: list[float] = []
    layer_begin_previous_us: list[float] = []

    per_frame: dict[int, dict[str, float]] = defaultdict(dict)
    for frame in frames:
        se = submit_ev.get(frame, {})
        ge = gpu_ev.get(frame, {})
        entry = i(se.get("handler_entry", {}), "event_ns")
        after_commit = i(se.get("after_commit", {}), "event_ns")
        ready = i(ge.get("semaphore_ready", {}), "event_ns")
        scheduled = i(ge.get("scheduled", {}), "event_ns")

        if entry and after_commit and after_commit >= entry:
            val = (after_commit - entry) / 1000.0
            handler_us.append(val)
            per_frame[frame]["handler_us"] = val
        if entry and ready and ready >= entry:
            val = (ready - entry) / 1000.0
            submit_to_ready_us.append(val)
            per_frame[frame]["submit_to_ready_us"] = val
        if "semaphore_ready" in ge:
            val = i(ge["semaphore_ready"], "duration_ns") / 1000.0
            semaphore_wait_us.append(val)
            per_frame[frame]["semaphore_wait_us"] = val
        if "scheduled" in ge:
            val = i(ge["scheduled"], "duration_ns") / 1000.0
            scheduled_wait_us.append(val)
            per_frame[frame]["scheduled_wait_us"] = val
        if entry and scheduled and scheduled >= entry:
            val = (scheduled - entry) / 1000.0
            submit_to_scheduled_us.append(val)
            per_frame[frame]["submit_to_scheduled_us"] = val
        if "layer_begin_after_previous" in ge:
            val = i(ge["layer_begin_after_previous"], "duration_ns") / 1000.0
            layer_begin_previous_us.append(val)
            per_frame[frame]["layer_begin_previous_us"] = val

    submit_display_lead_us = []
    for frame, events in submit_ev.items():
        row = events.get("handler_entry")
        if row:
            event_ns = i(row, "event_ns")
            display_ns = i(row, "display_time_ns")
            if event_ns and display_ns:
                submit_display_lead_us.append((display_ns - event_ns) / 1000.0)

    print("Client submit -> GPU-ready path")
    describe("display lead at native submit", submit_display_lead_us)
    describe("native handler", handler_us)
    describe("submit -> semaphore ready", submit_to_ready_us)
    describe("semaphore wait", semaphore_wait_us)
    describe("scheduled-slot wait", scheduled_wait_us)
    describe("submit -> scheduled", submit_to_scheduled_us)
    describe("layer_begin previous wait", layer_begin_previous_us)
    print()

    map_by_client: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in fmap:
        cf = i(row, "client_frame_id", -1)
        if cf >= 0:
            map_by_client[cf].append(row)

    ready_to_latch_us: list[float] = []
    source_uses: list[float] = []
    reused_system_frames = 0
    mapped_system_frames: set[int] = set()
    for frame, rows in map_by_client.items():
        rows.sort(key=lambda r: i(r, "latch_ns"))
        source_uses.append(float(len(rows)))
        reused_system_frames += sum(1 for r in rows if i(r, "reused") != 0)
        mapped_system_frames.update(i(r, "system_frame_id", -1) for r in rows if i(r, "system_frame_id", -1) >= 0)
        ready = i(gpu_ev.get(frame, {}).get("semaphore_ready", {}), "event_ns")
        latch = i(rows[0], "latch_ns") if rows else 0
        if ready and latch and latch >= ready:
            val = (latch - ready) / 1000.0
            ready_to_latch_us.append(val)
            per_frame[frame]["ready_to_first_latch_us"] = val

    print("GPU ready -> system compositor")
    describe("ready -> first latch", ready_to_latch_us)
    describe("system refreshes / client frame", source_uses, "x")
    print(f"Mapped system frames: {len(mapped_system_frames)}")
    print(f"Rows explicitly marked reused: {reused_system_frames}")
    print()

    present_by_system = {i(row, "frame_id", -1): row for row in present}
    presented_by_system = {i(row, "frame_id", -1): row for row in presented}

    drawable_stage_us: list[float] = []
    native_present_submit_us: list[float] = []
    presented_minus_target_us: list[float] = []
    presented_minus_desired_us: list[float] = []
    shared_event_wait_count = 0

    for sf in mapped_system_frames:
        prow = present_by_system.get(sf)
        if prow:
            host_call = i(prow, "host_call_ns")
            after_drawable = i(prow, "after_drawable_ns")
            after_commit = i(prow, "after_commit_ns")
            if host_call and after_drawable and after_drawable >= host_call:
                drawable_stage_us.append((after_drawable - host_call) / 1000.0)
            if host_call and after_commit and after_commit >= host_call:
                native_present_submit_us.append((after_commit - host_call) / 1000.0)
            shared_event_wait_count += 1 if i(prow, "shared_event_wait") else 0

        arow = presented_by_system.get(sf)
        if arow:
            presented_minus_target_us.append(i(arow, "presented_minus_target_ns") / 1000.0)
            presented_minus_desired_us.append(i(arow, "presented_minus_desired_ns") / 1000.0)

    print("System compositor -> Metal presentation")
    describe("present call -> drawable", drawable_stage_us)
    describe("present call -> commit", native_present_submit_us)
    describe("presented - target", presented_minus_target_us)
    describe("presented - desired", presented_minus_desired_us)
    if mapped_system_frames:
        print(f"Present rows using shared-event wait: {shared_event_wait_count}/{len(mapped_system_frames)}")
    for threshold_ms in (1, 4, 8):
        if presented_minus_target_us:
            threshold_us = threshold_ms * 1000.0
            count = sum(v > threshold_us for v in presented_minus_target_us)
            print(f"presented > target +{threshold_ms}ms: {count:5d} ({100*count/len(presented_minus_target_us):6.2f}%)")
    print()

    worst_ready = sorted(
        ((vals.get("submit_to_ready_us", 0.0), frame) for frame, vals in per_frame.items()),
        reverse=True,
    )[:10]
    print("Worst client submit -> GPU ready:")
    for value, frame in worst_ready:
        if value <= 0:
            continue
        vals = per_frame[frame]
        print(
            f"  client_frame={frame:6d} total={value:9.1f} us "
            f"handler={vals.get('handler_us', 0):8.1f} "
            f"sem_wait={vals.get('semaphore_wait_us', 0):8.1f} "
            f"ready_to_latch={vals.get('ready_to_first_latch_us', 0):8.1f}"
        )

    if swapchain:
        worst_waits = sorted(
            (
                (i(row, "duration_ns") / 1000.0, i(row, "swapchain_id"), i(row, "image_index"))
                for row in swapchain
                if row.get("event") == "wait"
            ),
            reverse=True,
        )[:10]
        print("\nWorst native xrWaitSwapchainImage calls:")
        for value, sc, image in worst_waits:
            print(f"  swapchain={sc:3d} image={image:2d} wait={value:9.1f} us")

    if presented_minus_target_us:
        late = sorted(
            (
                (i(row, "presented_minus_target_ns") / 1000.0, i(row, "frame_id", -1))
                for row in presented
                if i(row, "frame_id", -1) in mapped_system_frames
            ),
            reverse=True,
        )[:10]
        print("\nWorst actual presentations vs target:")
        for value, frame in late:
            print(f"  system_frame={frame:6d} presented_minus_target={value:9.1f} us")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
