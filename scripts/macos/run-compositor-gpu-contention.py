#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Run isolated and contended offscreen composition comparisons on the same GPU.

The competing process uses the production foveated distortion shader through
an independent Vulkan device/queue. No service or headset is started. Each load
worker has a 30-second safety deadline and is stopped gracefully after capture.
"""
import argparse
import csv
import json
import os
import statistics
import subprocess
import time
from collections import defaultdict
from pathlib import Path


def read_csv(path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def gpu_label(path):
    labels = {line for line in path.read_text().splitlines() if line.startswith("GPU:")}
    if len(labels) != 1:
        raise RuntimeError(f"Missing or inconsistent GPU metadata: {path}")
    return labels.pop()


def capture(binary, load_binary, output, mode, repeat):
    root = output / f"{mode}-{repeat}"
    root.mkdir()  # Never overwrite an existing measurement.
    env = os.environ.copy()
    env.pop("MONADO_DISTORTION_BENCH_SMALL", None)
    measurement = root / "composition.csv"
    env["MONADO_LAYER_BENCH_CSV"] = str(measurement)
    worker = None
    worker_log = None
    try:
        if mode != "idle":
            worker_env = env.copy()
            worker_env.update(MONADO_GPU_LOAD_MODE=mode,
                              MONADO_GPU_LOAD_CSV=str(root / "load.csv"),
                              MONADO_GPU_LOAD_STOP=str(root / "stop"))
            worker_log = (root / "load.log").open("w")
            worker = subprocess.Popen([str(load_binary), "[gpu_load]"], env=worker_env,
                                      stdout=worker_log, stderr=subprocess.STDOUT)
            ready_deadline = time.monotonic() + 15
            while True:
                if worker.poll() is not None:
                    raise RuntimeError(f"GPU load did not start; inspect {root / 'load.log'}")
                if (root / "load.csv").exists() and len(read_csv(root / "load.csv")) >= 5:
                    break
                if time.monotonic() > ready_deadline:
                    raise RuntimeError("GPU load readiness timeout")
                time.sleep(.05)
        with (root / "composition.log").open("w") as log:
            subprocess.run([str(binary), "[layer_benchmark]"], env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=25)
    finally:
        if worker is not None:
            (root / "stop").touch()
            try:
                worker.wait(timeout=10)
            except subprocess.TimeoutExpired:
                worker.terminate()
                try:
                    worker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.wait()
            if worker_log:
                worker_log.close()
    rows = read_csv(measurement)
    if len(rows) != 144:
        raise RuntimeError(f"Incomplete composition capture: {root}")
    result = {"mode": mode, "repeat": repeat, "samples": len(rows), "gpu": gpu_label(root / "composition.log"), "groups": {}}
    if worker is not None:
        if worker.returncode != 0:
            raise RuntimeError(f"Load worker failed: {root / 'load.log'}")
        if gpu_label(root / "load.log") != result["gpu"]:
            raise RuntimeError("Load worker selected different GPU metadata")
        load = read_csv(root / "load.csv")
        # CPU submission/completion brackets establish that the independent
        # queue is active during every measurement. They don't prove simultaneous
        # execution on GPU cores, or exclude scheduler serialisation.
        overlaps = []
        for row in rows:
            start, end = int(row["host_start_ns"]), int(row["host_end_ns"])
            hits = [r for r in load if int(r["host_start_ns"]) < end and int(r["host_end_ns"]) > start]
            overlaps.append(len(hits))
        overlap_fraction = sum(n > 0 for n in overlaps) / len(overlaps)
        if overlap_fraction == 0 or (mode == "saturated" and overlap_fraction < 1):
            raise RuntimeError(f"Insufficient overlapping load activity: {root}")
        start = min(int(r["host_start_ns"]) for r in rows)
        end = max(int(r["host_end_ns"]) for r in rows)
        relevant = [r for r in load if int(r["host_start_ns"]) < end and int(r["host_end_ns"]) > start]
        span = (max(int(r["host_end_ns"]) for r in relevant) - min(int(r["host_start_ns"]) for r in relevant)) / 1e9
        result["load"] = {"overlap_min_batches": min(overlaps), "overlap_fraction": overlap_fraction, "active_batches": len(relevant),
                          "dispatches_per_s": sum(int(r["dispatches"]) for r in relevant) / span,
                          "batch_elapsed_gpu_ms_median": statistics.median(float(r["gpu_batch_ms"]) for r in relevant),
                          "paced_deadline_overrun_fraction": (sum(int(r["host_end_ns"]) - int(r["host_start_ns"]) > 8333333 for r in relevant) / len(relevant)
                                                               if mode == "paced" else None)}
    groups = defaultdict(dict)
    for row in rows:
        groups[int(row["quads"]), int(row["correction"])][row["variant"], int(row["round"])] = float(row["gpu_ms"])
    for (quads, correction), values in groups.items():
        base = [values["baseline", i] for i in range(12)]
        candidate = [values["candidate", i] for i in range(12)]
        result["groups"][f"quads={quads},srgb={correction}"] = {
            "baseline_ms": statistics.median(base), "bounds_ms": statistics.median(candidate),
            "paired_saving_percent": 100 * (1 - statistics.median(c / b for b, c in zip(base, candidate)))}
    (root / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"{mode} repeat {repeat}: {json.dumps(result['groups'])}", flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, default=Path("build/arm64/tests/tests_render_compute"))
    p.add_argument("--load-binary", type=Path, default=Path("build/arm64/tests/tests_render_compute"))
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--repeats", type=int, default=2, choices=range(1, 5))
    args = p.parse_args()
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []
    # Counterbalance mode order as well as baseline/candidate order inside runs.
    for repeat in range(1, args.repeats + 1):
        modes = ["idle", "paced", "saturated"] if repeat % 2 else ["saturated", "paced", "idle"]
        for mode in modes:
            print(f"Running {mode}, repeat {repeat}", flush=True)
            results.append(capture(binary, args.load_binary.resolve(), output, mode, repeat))
    (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
