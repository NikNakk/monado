#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Measure physical PS VR2 presentation under independent GPU load.

Run only with the headset connected and other headset applications closed.
Each mode repeats the ordinary copy path with isolated services and finite workers.
This synthetic Metal quad plus compute competitor is not a game workload.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import importlib.util

spec = importlib.util.spec_from_file_location("gpu_contention", Path(__file__).with_name("run-compositor-gpu-contention.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


def distribution(values):
    values = sorted(values)
    return {"n": len(values), **{name: values[round((len(values) - 1) * q)] if values else None
                              for name, q in (("p50", .5), ("p95", .95), ("p99", .99), ("max", 1))}}


def trace(root, name):
    paths = list(root.glob(f"monado_psvr2_*_{name}.csv"))
    if len(paths) != 1:
        raise RuntimeError(f"Expected one {name} trace in {root}, found {len(paths)}")
    return helpers.read_csv(paths[0])


def summarize(root, mode, refresh_hz=120):
    physical = sorted([r for r in trace(root, "presented") if int(r["presented_monotonic_ns"]) > 0],
                      key=lambda r: int(r["presented_monotonic_ns"]))
    if len(physical) < 120:
        raise RuntimeError(f"Insufficient physical presentations: {root}")
    # Exclude the first and last second: startup/shutdown are retained in raw data.
    start = int(physical[0]["presented_monotonic_ns"]) + 1_000_000_000
    end = int(physical[-1]["presented_monotonic_ns"]) - 1_000_000_000
    physical = [r for r in physical if start <= int(r["presented_monotonic_ns"]) <= end]
    if len(physical) < 120:
        raise RuntimeError(f"Insufficient steady physical presentations: {root}")
    ids = {r["frame_id"] for r in physical}
    times = [int(r["presented_monotonic_ns"]) for r in physical]
    gaps = [(b-a)/1e6 for a, b in zip(times, times[1:])]
    nominal_ms = 1000 / refresh_hz
    workers = [r for r in trace(root, "present_worker") if r["frame_id"] in ids]
    submitted = [r for r in workers if r["event"] == "submitted"]
    requests = {r["frame_id"]: r for r in trace(root, "present")}
    completions = [r for r in trace(root, "present_complete") if r["frame_id"] in ids]
    if not completions or not submitted:
        raise RuntimeError(f"Missing GPU/worker lifecycle evidence: {root}")
    if any(int(r["status"]) != 4 for r in completions):  # MTLCommandBufferStatusCompleted
        raise RuntimeError(f"Metal command failure: {root}")
    result = {
        "run": root.name, "load": mode,
        "physical_frames": len(physical), "window_start_ns": start, "window_end_ns": end,
        "physical_hz": (len(times)-1)*1e9/(times[-1]-times[0]),
        "cadence_ms": distribution(gaps), "nominal_period_ms": nominal_ms, "configured_refresh_hz": refresh_hz,
        "gaps_over_1_5_period_fraction": sum(g > 1.5*nominal_ms for g in gaps)/len(gaps),
        "present_minus_desired_ms": distribution([int(r["presented_minus_desired_ns"])/1e6 for r in physical]),
        "commit_to_physical_ms": distribution([(int(r["presented_monotonic_ns"])-int(requests[r["frame_id"]]["after_commit_ns"]))/1e6
                                                for r in physical if r["frame_id"] in requests]),
        "worker_delay_ms": distribution([int(r["worker_delay_ns"])/1e6 for r in submitted]),
        "drawable_wait_ms": distribution([int(r["drawable_wait_ns"])/1e6 for r in submitted]),
        # This command can wait for Vulkan before its Metal work. It is not
        # comparable to the offscreen benchmark's isolated compute+blit sum.
        "metal_command_elapsed_ms": distribution([(float(r["gpu_end_time_s"])-float(r["gpu_start_time_s"]))*1000
                                                   for r in completions if float(r["gpu_end_time_s"]) > float(r["gpu_start_time_s"]) > 0]),
    }
    all_gpu = trace(root, "compositor_gpu")
    gpu = [r for r in all_gpu if r["frame_id"] in ids]
    result["vulkan_compositor_ms"] = distribution([int(r["gpu_duration_ns"])/1e6 if "gpu_duration_ns" in r
                                                   else (int(r["gpu_end_ns"])-int(r["gpu_start_ns"]))/1e6 for r in gpu])
    result["vulkan_all_feedback_ms"] = distribution([int(r["gpu_duration_ns"])/1e6 for r in all_gpu
                                                     if "gpu_duration_ns" in r and start <= int(r["feedback_ns"]) <= end])
    if mode != "idle":
        load = helpers.read_csv(root / "load.csv")
        active = [r for r in load if int(r["host_start_ns"]) < end and int(r["host_end_ns"]) > start]
        if not active or min(int(r["host_start_ns"]) for r in load) > start or max(int(r["host_end_ns"]) for r in load) < end:
            raise RuntimeError(f"Load worker did not cover measurement window: {root}")
        result["load_batches"] = len(active)
        result["load_gpu_batch_ms"] = distribution([float(r["gpu_batch_ms"]) for r in active])
        result["load_dispatches_per_s"] = sum(int(r["dispatches"]) for r in active) / ((end-start)/1e9)
        result["load_gpu"] = helpers.gpu_label(root / "load.log")
    (root / "presentation-summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def capture(args, root, mode, service_settings=None, client_mode="normal"):
    root.mkdir()
    worker = None
    try:
        if mode != "idle":
            env = os.environ.copy()
            env.update(MONADO_GPU_LOAD_MODE=mode, MONADO_GPU_LOAD_CSV=str(root / "load.csv"),
                       MONADO_GPU_LOAD_STOP=str(root / "stop"), MONADO_GPU_LOAD_SECONDS=str(args.seconds + 40))
            with (root / "load.log").open("w") as log:
                worker = subprocess.Popen([str(args.build / "tests/tests_render_compute"), "[gpu_load]"],
                                          env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 15
            while not ((root / "load.csv").exists() and len(helpers.read_csv(root / "load.csv")) >= 5):
                if worker.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f"Load worker failed readiness: {root}")
                time.sleep(.05)
        command = [sys.executable, str(Path(__file__).with_name("run-gpu-sync-service.py")),
                   "--build", str(args.build), "--headset", "--benchmark", "--output", str(root),
                   "--client-timeout", str(args.seconds + 25)]
        for key, value in (service_settings or {}).items():
            command += ["--service-env", f"{key}={value}"]
        command += ["--client", str(args.build / "tests/tests_macos_service_gpu_sync"),
                    client_mode, str(args.seconds * args.refresh_hz), str(args.seconds + 10)]
        # Wake connected displays and hold the assertion only for this capture.
        command = ["/usr/bin/caffeinate", "-d", "-u", *command]
        with (root / "runner.log").open("w") as log:
            # The service runner bounds startup/client/cleanup internally. Let
            # its finally block boot out launchd rather than kill it mid-cleanup.
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
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
    if worker is not None and worker.returncode != 0:
        raise RuntimeError(f"Load worker failed: {root}")
    return summarize(root, mode, args.refresh_hz)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--build", type=Path, default=Path("build/arm64"))
    p.add_argument("--seconds", type=int, default=20, choices=range(5, 91))
    p.add_argument("--refresh-hz", type=int, choices=[90, 120], default=120,
                   help="Actual configured headset refresh; cadence gaps use this fixed reference")
    p.add_argument("--modes", nargs="+", choices=["idle", "paced", "saturated"], default=["idle", "paced", "saturated"])
    p.add_argument("--quads", type=int, choices=[1, 4], default=1)
    p.add_argument("--quad-bounds", action="store_true", help="Enable conservative quad intersection bounds in the compositor")
    args = p.parse_args()
    args.build = args.build.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    for mode in args.modes:
        for number in range(1, 3):
            root = args.output / f"{mode}-{number}-copy"
            print(f"Running {root.name}", flush=True)
            settings = {"XRT_MACOS_COMPOSITOR_QUAD_BOUNDS": "1" if args.quad_bounds else "0"}
            result = capture(args, root, mode, settings, "quads-four" if args.quads == 4 else "normal")
            result["quads"] = args.quads
            result["quad_bounds_enabled"] = args.quad_bounds
            results.append(result)
            (args.output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
            print(f"{result['physical_hz']:.2f} Hz; gaps {100*result['gaps_over_1_5_period_fraction']:.2f}%; "
                  f"Vulkan median {result['vulkan_compositor_ms']['p50']} ms", flush=True)


if __name__ == "__main__":
    main()
