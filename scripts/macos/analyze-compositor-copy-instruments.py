#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Measure CA-copy producer/consumer queuing in one exported Instruments run.

Uses command-buffer/encoder IDs and MonadoCopy labels. Instruments frame numbers
are not Monado frame IDs. Two-compute-encoder classification is only appropriate for the declared
two-eye four-quad workload: both composition dispatches share an encoder,
then the image barrier separates the distortion encoder.
"""
import argparse
import collections
import importlib.util
import json
import re
from pathlib import Path

spec = importlib.util.spec_from_file_location("phase", Path(__file__).with_name("analyse-instruments-display-phase.py"))
phase = importlib.util.module_from_spec(spec)
spec.loader.exec_module(phase)


def distribution(values):
    values = sorted(values)
    if not values:
        return {"n": 0}
    return {"n": len(values), "min": values[0], "p50": values[int(.5 * (len(values) - 1))],
            "p95": values[int(.95 * (len(values) - 1))], "p99": values[int(.99 * (len(values) - 1))],
            "max": values[-1]}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exports", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    commands = collections.defaultdict(dict)
    consumers = collections.defaultdict(dict)
    producers = collections.defaultdict(dict)
    gpu = collections.defaultdict(dict)
    encoders = collections.defaultdict(dict)
    cpu_encoders = collections.defaultdict(dict)
    present = collections.defaultdict(dict)
    label_pattern = re.compile(r"MonadoCopy frame=(\d+) timeline=(\d+) image=(\d+)")

    def rows(schema):
        return phase.rows(a.exports / (schema + ".xml"))

    def integer(row, field):
        return int(row[field][1])

    for row in rows("metal-application-command-buffer-submissions"):
        process = row["process"][0]
        if "monado-service" not in process:
            continue
        key = integer(row, "cmdbuffer-id")
        start = integer(row, "start")
        command = (start, start + integer(row, "duration"), key)
        commands[process][key] = command
        label = label_pattern.search(row["event-label"][0])
        if label:
            consumers[process][key] = {"command": command, "frame_id": label.group(1),
                                       "timeline_value": label.group(2), "image": label.group(3)}
        elif "vkQueueSubmit MTLCommandBuffer on Queue 0-0" in row["event-label"][0]:
            producers[process][key] = command
    for row in rows("metal-application-intervals"):
        process = row["process"][0]
        if process not in consumers or "vkCmdDispatch" not in row["event-label"][0]:
            continue
        command_id, encoder_id = integer(row, "cmdbuffer-id"), integer(row, "encoder-id")
        start = integer(row, "start")
        key = (command_id, encoder_id)
        cpu_encoders[process][key] = min(cpu_encoders[process].get(key, start), start)
    for row in rows("metal-gpu-intervals"):
        process = row["process"][0]
        if process not in consumers or row["state"][0] != "Active":
            continue
        command_id, encoder_id = integer(row, "cmdbuffer-id"), integer(row, "encoder-id")
        start = integer(row, "start")
        end = start + integer(row, "duration")
        old = gpu[process].get(command_id, (start, end))
        gpu[process][command_id] = (min(old[0], start), max(old[1], end))
        key = (command_id, encoder_id)
        old = encoders[process].get(key, (start, end))
        encoders[process][key] = (min(old[0], start), max(old[1], end))
    handlers = collections.defaultdict(list)
    api_calls = collections.defaultdict(list)
    for row in rows("metal-application-event-interval"):
        process = row["process"][0]
        if process not in consumers:
            continue
        start = integer(row, "start")
        label = label_pattern.search(row["event-label"][0])
        if label and "Scheduled Handler" in row["event-label"][0]:
            handlers[process].append((start, start + integer(row, "duration"), row["thread"][0], label.group(1)))
        if row["event-type"][0] == "Drawable Present":
            api_calls[process].append((start, row["thread"][0]))
    import bisect
    for process, intervals in handlers.items():
        intervals.sort()
        starts = [item[0] for item in intervals]
        for start, thread in api_calls[process]:
            i = bisect.bisect_right(starts, start) - 1
            if i >= 0 and start <= intervals[i][1] and thread == intervals[i][2]:
                present[process][intervals[i][3]] = start

    result = {"units": "milliseconds", "window": "first/last labelled consumer commit trimmed by one second",
              "clock": "all latency differences remain within the Instruments trace clock",
              "pass_queries": False, "processes": []}
    for process in sorted(consumers, key=lambda proc: min(item["command"][0] for item in consumers[proc].values())):
        consumer_list = list(consumers[process].values())
        lo = min(item["command"][0] for item in consumer_list) + 1_000_000_000
        hi = max(item["command"][0] for item in consumer_list) - 1_000_000_000
        if hi <= lo:
            raise RuntimeError("Insufficient steady trace window")
        samples = collections.defaultdict(list)
        compute_encoder_counts = collections.Counter()
        producer_count = 0
        for command_id, (start, end, _) in producers[process].items():
            if not lo <= start <= hi or command_id not in gpu[process]:
                continue
            producer_count += 1
            gpu_start, gpu_end = gpu[process][command_id]
            samples["producer_commit_to_gpu_start_ms"].append((gpu_start - end) / 1e6)
            samples["producer_gpu_span_ms"].append((gpu_end - gpu_start) / 1e6)
            dispatches = sorted((time, encoders[process][key])
                                for key, time in cpu_encoders[process].items()
                                if key[0] == command_id and key in encoders[process])
            compute_encoder_counts[len(dispatches)] += 1
            if len(dispatches) == 2:
                composition = [item[1] for item in dispatches[:1]]
                distortion = dispatches[1][1]
                samples["composition_encoder_span_ms"].append((max(e for _, e in composition) - min(s for s, _ in composition)) / 1e6)
                samples["distortion_encoder_span_ms"].append((distortion[1] - distortion[0]) / 1e6)
        for command_id, item in consumers[process].items():
            start, end, _ = item["command"]
            if not lo <= start <= hi or command_id not in gpu[process]:
                continue
            gpu_start, gpu_end = gpu[process][command_id]
            samples["consumer_commit_to_copy_gpu_start_ms"].append((gpu_start - end) / 1e6)
            samples["copy_gpu_encoder_span_ms"].append((gpu_end - gpu_start) / 1e6)
            if item["frame_id"] in present[process]:
                samples["consumer_commit_to_actual_present_ms"].append((present[process][item["frame_id"]] - end) / 1e6)
        result["processes"].append({"process": process, "labelled_consumer_commands": len(consumer_list),
                                    "steady_producer_commands_with_gpu_ids": producer_count,
                                    "compute_encoder_counts": dict(compute_encoder_counts),
                                    "distributions": {name: distribution(values) for name, values in samples.items()}})
    result["limitations"] = [
        "Encoder spans include preemption/gaps between Active intervals; they are not sums of exclusive GPU occupancy.",
        "Consumer commit-to-copy time includes the explicit producer shared-event wait and scheduling; it is not isolated scheduler overhead.",
        "Presentation API calls are instant trace events, not physical display timestamps or private WindowServer latch times.",
        "Pass classification applies only to commands with two observed compute encoders in the fixed four-quad workload; unexpected counts remain explicit.",
        "Trace profiling changes scheduling; use the untraced matrix for cadence and display-delay acceptance.",
    ]
    a.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
