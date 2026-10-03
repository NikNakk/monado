#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Analyse capture-tracking-freshness.py output without changing clock mapping.

Reports excess delivery delay against both a trailing five-second lower envelope
and a session-wide fast-delivery baseline. Neither estimates absolute USB latency.
The short envelope can hide sustained delay; the session floor can include clock
drift. Compare both and check the service's external-background policy timeline.

Writes freshness-summary.json and pose-presented.csv in the capture directory.
"""
import argparse
import bisect
import collections
import csv
import importlib.util
import json
import math
import pathlib

SPEC = importlib.util.spec_from_file_location("delivery", pathlib.Path(__file__).parents[1] / "psvr2_delivery_delay.py")
DELIVERY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DELIVERY)


def stats(values):
    values = sorted(values)
    if not values:
        return {"n": 0, "p50": None, "p95": None, "p99": None, "max": None}
    return {"n": len(values), **{key: values[round((len(values) - 1) * q)]
                                 for key, q in (("p50", .5), ("p95", .95), ("p99", .99), ("max", 1))}}


def quaternion_distance_deg(a, b):
    # q and -q encode the same rotation. Reject invalid/fallback poses.
    aa, bb = sum(x * x for x in a), sum(x * x for x in b)
    if not all(math.isfinite(x) for x in (*a, *b)) or aa <= 0 or bb <= 0:
        return None
    dot = abs(sum(x * y for x, y in zip(a, b))) / math.sqrt(aa * bb)
    return math.degrees(2 * math.acos(min(1, dot)))


def delay_series(times, device, window_ns):
    if not times:
        raise ValueError("Empty sensor stream")
    if any(b < a for a, b in zip(times, times[1:])):
        raise ValueError("Host timestamps regress")
    if any(b < a for a, b in zip(device, device[1:])):
        raise ValueError("Device timestamps regress: split into clock epochs before analysis")
    difference = [host - vts for host, vts in zip(times, device)]
    floor = DELIVERY.rolling_min(times, difference, window_ns)
    session_floor = min(difference)
    return floor, session_floor, [d - f for d, f in zip(difference, floor)], [d - session_floor for d in difference]


def callback_batches(imu):
    """Keep the newest device sample delivered by each actual USB callback.

    Backspread sample estimates may overlap between batches; the actual
    callback is ordered. Older samples in a batch include intentional batching
    age and must not be counted as separate late USB deliveries.
    """
    batches = []
    for row in imu:
        if batches and row[1] == batches[-1][1]:
            if row[2] >= batches[-1][2]:
                batches[-1] = row
        else:
            batches.append(row)
    return batches


def raw_age(query_ns, vts_ns, floor_ns):
    return (query_ns - vts_ns - floor_ns) / 1e6


def last_at(times, values, query):
    index = bisect.bisect_right(times, query) - 1
    return values[index] if index >= 0 else None


def read_rows(path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def policy_timeline(out):
    times, states = [], []
    for line in (out / "policy.jsonl").read_text().splitlines():
        row = json.loads(line)
        policy = row["processes"]["server"]["policy"]
        # This is external backgrounding, not a direct Game Mode bit.
        state = (True if "ext_darwinbg=1" in policy else
                 False if "ext_darwinbg=0" in policy else None)
        times.append(row["host_ns"])
        states.append(state)
    return times, states


def analyze(out, window_s=5):
    process = json.loads((out / "process.json").read_text())
    window = json.loads((out / "window.json").read_text())
    if window["clock"] != "CLOCK_MONOTONIC":
        raise ValueError("Capture window is not in Monado's clock domain")
    begin, end = window["start_ns"], window["end_ns"]
    client, server = process["client_pid"], process["server_pid"]
    def path(side, pid, name):
        return out / side / f"monado_psvr2_{pid}_{name}.csv"
    imu = []
    with path("server", server, "imu").open(newline="") as f:
        for row in csv.DictReader(f):
            speed = math.sqrt(sum(float(row[f"gyro_{axis}"]) ** 2 for axis in "xyz"))
            imu.append((int(row["host_estimated_sample_ns"]), int(row["host_callback_ns"]),
                        int(row["vts_ns"]), int(row["vts_mapped_host_ns"]), speed))
    slam = [(int(row["host_received_ns"]), int(row["slam_vts_ns"]))
            for row in read_rows(path("server", server, "slam"))]
    raw_imu = imu
    imu = callback_batches(raw_imu)
    times = [row[1] for row in imu]
    vts = [row[2] for row in imu]
    floor, session_floor, rolling, session = delay_series(times, vts, int(window_s * 1e9))
    policy_times, policy_states = policy_timeline(out)
    result = {"motion_requested": process["motion"], "game_mode": process["game_mode"],
              "window": window, "envelope_window_s": window_s,
              "method": "Excess delay relative to prompt-delivery baselines; no absolute latency or clock correction claimed."}
    def in_window(t):
        return begin <= t <= end
    selection = [i for i, row in enumerate(imu) if in_window(row[1])]
    callbacks = sorted(set(row[1] for row in imu if in_window(row[1])))
    result["imu"] = {
        "sample_count": sum(in_window(row[1]) for row in raw_imu),
        "callback_count": len(selection),
        "delivery_excess_rolling_ms": stats([rolling[i] / 1e6 for i in selection]),
        "delivery_excess_session_ms": stats([session[i] / 1e6 for i in selection]),
        "mapping_above_rolling_floor_ms": stats([(imu[i][3] - imu[i][2] - floor[i]) / 1e6 for i in selection]),
        "mapping_above_session_floor_ms": stats([(imu[i][3] - imu[i][2] - session_floor) / 1e6 for i in selection]),
        "callback_gaps_ms": stats([(b - a) / 1e6 for a, b in zip(callbacks, callbacks[1:])]),
        "gyro_speed_deg_s": stats([math.degrees(imu[i][4]) for i in selection]),
        "moving_fraction_above_0_1_rad_s": sum(imu[i][4] > .1 for i in selection) / len(selection) if selection else None,
        "floor_drift_range_ms": (max(floor[i] for i in selection) - min(floor[i] for i in selection)) / 1e6 if selection else None,
    }
    st = [r[0] for r in slam]
    sf, ss, sr, sa = delay_series(st, [r[1] for r in slam], int(window_s * 1e9))
    chosen = [i for i, t in enumerate(st) if in_window(t)]
    result["slam"] = {
        "sample_count": len(chosen),
        "delivery_excess_rolling_ms": stats([sr[i] / 1e6 for i in chosen]),
        "delivery_excess_session_ms": stats([sa[i] / 1e6 for i in chosen]),
        "callback_gaps_ms": stats([(st[b] - st[a]) / 1e6 for a, b in zip(chosen, chosen[1:])]),
    }
    groups = collections.defaultdict(list)
    for i in selection:
        time_ns = imu[i][1]
        state = last_at(policy_times, policy_states, time_ns)
        index = bisect.bisect_right(policy_times, time_ns) - 1
        if index < 0 or time_ns - policy_times[index] > 3_000_000_000:
            state = None
        label = "externally_backgrounded" if state is True else "not_externally_backgrounded" if state is False else "unknown"
        groups[label].append(session[i] / 1e6)
    result["imu_delivery_by_service_policy_ms"] = {key: stats(values) for key, values in groups.items()}
    shared = read_rows(path("client", client, "shared_tracking"))
    shared.sort(key=lambda r: int(r["query_ns"]))
    queries = [r for r in shared if in_window(int(r["query_ns"]))]
    extra_age, mapping_shift = [], []
    for row in queries:
        source_host = int(row["imu_callback_ns"])
        envelope = last_at(times, floor, source_host)
        if envelope is not None and int(row["imu_vts_ns"]) > 0:
            extra_age.append(raw_age(int(row["query_ns"]), int(row["imu_vts_ns"]), envelope))
            mapping_shift.append((int(row["hw2mono_vts_ns"]) - envelope) / 1e6)
    result["client"] = {
        "queries": len(queries), "rpc_fallbacks": sum(int(r["rpc_fallback"]) for r in queries),
        "bounded_read_misses": sum(int(r["read_miss"]) for r in queries),
        "flags": dict(collections.Counter(r["flags"] for r in queries)),
        "query_ms": stats([(int(r["end_ns"]) - int(r["query_ns"])) / 1e6 for r in queries]),
        "imu_callback_age_ms": stats([(int(r["query_ns"]) - int(r["imu_callback_ns"])) / 1e6 for r in queries if int(r["imu_callback_ns"]) > 0]),
        "slam_callback_age_ms": stats([(int(r["query_ns"]) - int(r["slam_received_ns"])) / 1e6 for r in queries if int(r["slam_received_ns"]) > 0]),
        "publication_age_ms": stats([(int(r["query_ns"]) - int(r["published_ns"])) / 1e6 for r in queries if int(r["published_ns"]) > 0]),
        "mapped_imu_age_ms": stats([(int(r["query_ns"]) - int(r["imu_host_ns"])) / 1e6 for r in queries]),
        "imu_age_vs_rolling_fast_delivery_ms": stats(extra_age),
        "imu_age_vs_session_fast_delivery_ms": stats([raw_age(int(r["query_ns"]), int(r["imu_vts_ns"]), session_floor) for r in queries if int(r["imu_vts_ns"]) > 0]),
        "mapping_shift_vs_rolling_floor_ms": stats(mapping_shift),
    }
    late = {row["frame_id"]: row for row in read_rows(path("client", client, "late_render"))}
    presented = [row for row in read_rows(path("client", client, "presented")) if in_window(int(row["presented_monotonic_ns"]))]
    physical_times = sorted(int(r["presented_monotonic_ns"]) for r in presented)
    if not physical_times:
        raise ValueError("No physical presentations in the measurement window")
    all_physical = [int(r["presented_monotonic_ns"]) for r in read_rows(path("client", client, "presented")) if int(r["presented_monotonic_ns"]) > 0]
    if min(all_physical) > begin or max(all_physical) < end:
        raise ValueError("Physical trace does not cover the full capture window")
    gaps = [(b - a) / 1e6 for a, b in zip(physical_times, physical_times[1:])]
    result["physical"] = {
        "hz": (len(physical_times) - 1) * 1e9 / (physical_times[-1] - physical_times[0]) if len(physical_times) > 1 else None,
        "interval_ms": stats(gaps), "over12pct": 100 * sum(g > 12 for g in gaps) / len(gaps) if gaps else None,
    }
    query_times = [int(r["query_ns"]) for r in shared]
    joined = []
    for present in presented:
        record = late.get(present["frame_id"])
        if not record:
            continue
        query_begin, query_end = int(record["pose_query_begin_ns"]), int(record["pose_query_end_ns"])
        a = bisect.bisect_left(query_times, query_begin)
        b = bisect.bisect_right(query_times, query_end)
        candidates = [r for r in shared[a:b] if r["rpc_fallback"] == "0"]
        target = int(record["predicted_display_ns"])
        exact = [r for r in candidates if int(r["target_ns"]) == target]
        # Never silently match a pose predicted for another scanout endpoint.
        if not exact:
            continue
        pose = exact[0]
        physical = int(present["presented_monotonic_ns"])
        joined.append({"frame_id": present["frame_id"], "physical_ns": physical,
                       "physical_minus_pose_target_ms": (physical - target) / 1e6,
                       "physical_minus_query_ms": (physical - int(pose["query_ns"])) / 1e6, **pose})
    result["pose_join"] = {"physical_frames": len(presented), "exact_target_matches": len(joined),
                           "physical_minus_pose_target_ms": stats([r["physical_minus_pose_target_ms"] for r in joined])}
    reprojection_path = path("client", client, "reprojection")
    if reprojection_path.exists():
        renderer = {r["system_frame_id"]: r for r in read_rows(reprojection_path)}
        differences, source_angles = [], []
        target_time_mismatches = 0
        for pose in joined:
            row = renderer.get(pose["frame_id"])
            if row is None or row["source_valid"] != "1":
                continue
            if int(row["predicted_display_ns"]) != int(pose["target_ns"]):
                target_time_mismatches += 1
                continue
            query = [float(pose[k]) for k in ("qw", "qx", "qy", "qz")]
            begin = [float(row["left_begin_" + k]) for k in ("qw", "qx", "qy", "qz")]
            difference = quaternion_distance_deg(query, begin)
            if difference is not None:
                differences.append(difference)
                source_angles.append(float(row["left_src_to_begin_deg"]))
        result["renderer_pose"] = {
            "method": "PS VR2 left-eye begin orientation versus the exact joined head query; eye orientation is identity. Stationary source/target equality alone does not imply an override.",
            "compared_frames": len(differences), "target_time_mismatches": target_time_mismatches,
            "begin_vs_query_deg": stats(differences),
            "begin_vs_query_over_0_001_deg": sum(d > .001 for d in differences),
            "source_to_begin_deg": stats(source_angles),
        }
    if joined:
        with (out / "pose-presented.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(joined[0]))
            writer.writeheader()
            writer.writerows(joined)
    (out / "freshness-summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--window-s", type=float, default=5)
    args = parser.parse_args()
    if args.window_s <= 0:
        parser.error("--window-s must be positive")
    print(json.dumps(analyze(args.capture, args.window_s), indent=2))


if __name__ == "__main__":
    main()
