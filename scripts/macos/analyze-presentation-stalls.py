#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Join buffered capture lifecycle traces without changing presentation policy.

An observed scheduled callback is not Metal's private timed-present callback.
GPU end times and presentedTime share the host clock; each presented row supplies
its measured host-to-Monado offset. Coincident gaps identify stages, not causes.
Old captures without scheduled/pump traces remain analysable with explicit gaps.
"""
import argparse
import bisect
import csv
import json
import math
import pathlib


def stats(values):
    values = sorted(values)
    return {"n": len(values), **{name: values[round((len(values) - 1) * q)] if values else None
                                 for name, q in (("p50", .5), ("p95", .95), ("p99", .99), ("max", 1))}}


def host_seconds_to_monotonic(seconds, presented):
    """Use this physical row's clock conversion, not its handler arrival time."""
    physical = int(presented["presented_monotonic_ns"])
    host = float(presented["presented_time_host_s"])
    if physical <= 0 or host <= 0 or not math.isfinite(seconds) or seconds <= 0:
        return None
    return round(seconds * 1e9) + physical - round(host * 1e9)


def gpu_end_ns(complete, presented, commit_ns):
    value = host_seconds_to_monotonic(float(complete["gpu_end_time_s"]), presented)
    # Validate the clock conversion against independently recorded CPU lifecycle.
    if value is None or value < commit_ns - 2_000_000 or value > int(complete["completion_handler_ns"]) + 2_000_000:
        return None
    return value


def analyze(out, threshold_ms=20):
    metadata = json.loads((out / "process.json").read_text())
    window = json.loads((out / "window.json").read_text())
    if window["clock"] != "CLOCK_MONOTONIC":
        raise ValueError("Unknown window clock")
    start, end = window["start_ns"], window["end_ns"]
    missing = []
    def rows(side, name):
        pid = metadata[f"{'client' if side == 'client' else 'server'}_pid"]
        path = out / side / f"monado_psvr2_{pid}_{name}.csv"
        if not path.exists():
            missing.append(name)
            return []
        with path.open(newline="") as f:
            return list(csv.DictReader(f))
    all_physical = rows("client", "presented")
    physical = sorted([r for r in all_physical if start <= int(r["presented_monotonic_ns"]) <= end],
                      key=lambda r: int(r["presented_monotonic_ns"]))
    if not physical:
        raise ValueError("No physical presentations in window")
    complete = {r["frame_id"]: r for r in rows("client", "present_complete")}
    scheduled = {r["frame_id"]: r for r in rows("client", "present_scheduled")}
    present = {r["frame_id"]: r for r in rows("client", "present")}
    ca = rows("client", "ca_callback")
    prefetch = [r for r in rows("client", "drawable_prefetch") if r["event"] == "slot_ready"]
    pump = rows("server", "appkit_pump")
    sensor = rows("server", "imu")
    callback_times = sorted(set(int(r["host_callback_ns"]) for r in sensor)) if sensor and "host_callback_ns" in sensor[0] else []
    policy = [json.loads(line) for line in (out / "policy.jsonl").read_text().splitlines()]
    policy_times = [r["host_ns"] for r in policy]
    joined = []
    for row in physical:
        frame = row["frame_id"]
        c, s, request = complete.get(frame), scheduled.get(frame), present.get(frame)
        if not c or not request:
            continue
        physical_ns = int(row["presented_monotonic_ns"])
        commit = int(request["after_commit_ns"])
        gpu_end = gpu_end_ns(c, row, commit)
        scheduled_ns = int(s["scheduled_callback_ns"]) if s else None
        joined.append({"frame_id": frame, "physical_ns": physical_ns, "seconds": (physical_ns - start) / 1e9,
                       "commit_ns": commit, "scheduled_callback_ns": scheduled_ns,
                       "completion_handler_ns": int(c["completion_handler_ns"]), "gpu_end_ns": gpu_end,
                       "commit_to_scheduled_callback_ms": (scheduled_ns - commit) / 1e6 if s else None,
                       "commit_to_completion_callback_ms": (int(c["completion_handler_ns"]) - commit) / 1e6,
                       "completion_callback_to_physical_ms": (physical_ns - int(c["completion_handler_ns"])) / 1e6,
                       "gpu_end_to_physical_ms": (physical_ns - gpu_end) / 1e6 if gpu_end else None,
                       "scheduled_callback_to_physical_ms": (physical_ns - scheduled_ns) / 1e6 if s else None,
                       "gpu_duration_ms": (float(c["gpu_end_time_s"]) - float(c["gpu_start_time_s"])) * 1000,
                       "minimum_duration_us": int(s["minimum_duration_us"]) if s else None,
                       "presents_with_transaction": int(s["presents_with_transaction"]) if s else None})
    by_frame = {r["frame_id"]: r for r in joined}
    hitches = []
    for previous, current in zip(physical, physical[1:]):
        a, b = int(previous["presented_monotonic_ns"]), int(current["presented_monotonic_ns"])
        if (b - a) / 1e6 < threshold_ms:
            continue
        callbacks = [r for r in ca if a <= int(r["callback_entry_ns"]) <= b]
        drawable = [r for r in prefetch if int(r["next_drawable_begin_ns"]) <= b + 10_000_000
                    and int(r["next_drawable_end_ns"]) >= a]
        pumps = [r for r in pump if int(r["end_ns"]) >= a and int(r["previous_end_ns"]) <= b]
        # Receipt gaps crossing the physical interval, including callbacks after it.
        sensor_gaps = [(y - x) / 1e6 for x, y in zip(callback_times, callback_times[1:]) if x <= b and y >= a]
        i = bisect.bisect_right(policy_times, b) - 1
        policy_state = policy[i]["processes"]["server"]["policy"] if i >= 0 and b - policy_times[i] <= 3_000_000_000 else "unknown"
        hitches.append({"frame_id": current["frame_id"], "seconds": (b - start) / 1e9,
                        "physical_gap_ms": (b - a) / 1e6, "frame_lifecycle": by_frame.get(current["frame_id"]),
                        "ca_callback_count_in_gap": len(callbacks),
                        "ca_callback_interval_max_ms": max((int(r["callback_interval_ns"]) / 1e6 for r in callbacks), default=None),
                        "overlapping_drawable_wait_max_ms": max((int(r["drawable_wait_ns"]) / 1e6 for r in drawable), default=None),
                        "service_appkit_duration_max_ms": max(((int(r["end_ns"]) - int(r["begin_ns"])) / 1e6 for r in pumps), default=None),
                        "service_between_pumps_max_ms": max(((int(r["begin_ns"]) - int(r["previous_end_ns"])) / 1e6 for r in pumps if int(r["previous_end_ns"]) > 0), default=None),
                        "overlapping_sensor_callback_gap_max_ms": max(sensor_gaps, default=None),
                        "service_policy": policy_state})
    metrics = ["commit_to_scheduled_callback_ms", "commit_to_completion_callback_ms", "completion_callback_to_physical_ms",
               "gpu_end_to_physical_ms", "scheduled_callback_to_physical_ms", "gpu_duration_ms"]
    result = {"window": window, "physical_frames": len(physical), "lifecycle_joins": len(joined),
              "missing_traces": missing, "threshold_ms": threshold_ms,
              "method": "Passive callback timing and host-clock GPU end; correlated waits are not root-cause attribution.",
              "summary": {key: stats([r[key] for r in joined if r[key] is not None]) for key in metrics},
              "gpu_clock_validation_failures": sum(r["gpu_end_ns"] is None for r in joined),
              "hitches": sorted(hitches, key=lambda r: r["physical_gap_ms"], reverse=True),
              "late_completed_frames": sorted([r for r in joined if r["gpu_end_to_physical_ms"] is not None
                                               and r["gpu_end_to_physical_ms"] >= threshold_ms],
                                              key=lambda r: r["gpu_end_to_physical_ms"], reverse=True)}
    (out / "presentation-stalls.json").write_text(json.dumps(result, indent=2) + "\n")
    if joined:
        with (out / "presentation-lifecycle.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(joined[0]))
            writer.writeheader()
            writer.writerows(joined)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--threshold-ms", type=float, default=20)
    args = parser.parse_args()
    if not math.isfinite(args.threshold_ms) or args.threshold_ms <= 0:
        parser.error("threshold must be positive and finite")
    result = analyze(args.capture, args.threshold_ms)
    print(json.dumps({key: result[key] for key in ["physical_frames", "lifecycle_joins", "missing_traces", "summary", "gpu_clock_validation_failures"]}, indent=2))
    print(f"Hitches: {len(result['hitches'])}; late completed frames: {len(result['late_completed_frames'])}")


if __name__ == "__main__":
    main()
