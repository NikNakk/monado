#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Join failure-triggered Wine ownership histories to OpenVR call intervals."""
import argparse
import collections
import json
import re
import statistics
from pathlib import Path


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def overlap(intervals, begin, end):
    """Union overlapping/nested proxy calls rather than double-counting them."""
    merged = []
    for a, b in sorted((max(begin, a), min(end, b)) for a, b in intervals if a < end and b > begin):
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(b, merged[-1][1])
        else:
            merged.append([a, b])
    return sum(b - a for a, b in merged)


def analyze(trace, history):
    calls = collections.defaultdict(lambda: collections.defaultdict(list))
    flushes = collections.defaultdict(list)
    for line in trace.splitlines():
        d = fields(line)
        if line.startswith("call="):
            a, b = int(d["begin_us"]), int(d["end_us"])
            if b < a:
                raise ValueError("Reversed call interval")
            calls[d["pid"]][d["call"]].append((a, b))
        elif line.startswith("trace_flush"):
            flushes[d["pid"]].append((int(d["begin_us"]), int(d["end_us"])))
    if not calls:
        raise ValueError("No OpenVR call intervals")
    windows = {pid: (min(a for v in kinds.values() for a, _ in v),
                     max(b for v in kinds.values() for _, b in v)) for pid, kinds in calls.items()}
    events = {}
    for line in history.splitlines():
        if line.startswith("mwxr-keyed tick_us="):
            d = fields(line)
            # History rings are reprinted on successive errors.
            events[tuple(sorted(d.items()))] = d
    releases, timeouts = [], []
    excluded_flush = 0
    for d in events.values():
        is_release = d["op"] == "release" and int(d["status"], 0) == 0
        is_timeout = d["op"] == "acquire" and int(d["status"], 0) == 0x102
        if not (is_release or is_timeout):
            continue
        pid = d["owner_pid"]
        if pid not in calls:
            continue
        end, held = int(d["tick_us"]), int(d["held_us"])
        begin = end - held
        if held <= 0 or begin < windows[pid][0] or end > windows[pid][1]:
            continue
        if overlap(flushes[pid], begin, end):
            excluded_flush += 1
            continue
        by_call = {kind: overlap(v, begin, end) / 1000 for kind, v in calls[pid].items()}
        pose = by_call.get("WaitGetPoses", 0)
        row = {"owner_pid": pid, "handle": d["handle"], "end_us": end, "held_ms": held / 1000,
               "wait_get_poses_ms": pose, "wait_fraction": pose * 1000 / held,
               "call_overlap_ms": by_call}
        (releases if is_release else timeouts).append(row)
    def summary(rows):
        return {"samples": len(rows), **({"median_held_ms": statistics.median(r["held_ms"] for r in rows),
                "median_wait_get_poses_ms": statistics.median(r["wait_get_poses_ms"] for r in rows),
                "median_wait_fraction": statistics.median(r["wait_fraction"] for r in rows)} if rows else {})}
    return {"limitation": "Failure-triggered history windows, not all-frame statistics; overlap is not causation or CPU attribution.",
            "releases": summary(releases), "timeouts_with_traced_owner": summary(timeouts),
            "excluded_flush_intervals": excluded_flush, "release_intervals": releases,
            "timeout_intervals": timeouts}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("trace", type=Path)
    p.add_argument("history", type=Path)
    p.add_argument("--shared-clock", action="store_true", help="Confirm trace QPC and Wine tick_us have the same epoch")
    p.add_argument("--output", type=Path)
    a = p.parse_args()
    if not a.shared_clock:
        p.error("Verify the QPC/Wine shared clock before joining, then pass --shared-clock")
    result = analyze(a.trace.read_text(errors="replace"), a.history.read_text(errors="replace"))
    rendered = json.dumps(result, indent=2) + "\n"
    if a.output:
        a.output.write_text(rendered)
    print(json.dumps({k: v for k, v in result.items() if not isinstance(v, list)}, indent=2))


if __name__ == "__main__":
    main()
