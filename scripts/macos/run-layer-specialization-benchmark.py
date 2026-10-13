#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Measure common projection-plus-quads specialization.

The independent finite load worker and benchmark use the same os_monotonic clock.
CPU interval overlap establishes competing work, not simultaneous GPU execution.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import statistics
import subprocess
import time
from pathlib import Path

spec = importlib.util.spec_from_file_location("contention", Path(__file__).with_name("run-compositor-gpu-contention.py"))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


def capture(binary, root, mode):
    root.mkdir()
    env = os.environ.copy()
    env["MONADO_LAYER_SPECIAL_CSV"] = str(root / "benchmark.csv")
    worker = None
    try:
        if mode != "idle":
            load_env = env.copy()
            load_env.update(MONADO_GPU_LOAD_MODE=mode, MONADO_GPU_LOAD_CSV=str(root / "load.csv"),
                            MONADO_GPU_LOAD_STOP=str(root / "stop"), MONADO_GPU_LOAD_SECONDS="30")
            with (root / "load.log").open("w") as log:
                worker = subprocess.Popen([str(binary), "[gpu_load]"], env=load_env,
                                          stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 15
            while not ((root / "load.csv").exists() and len(h.read_csv(root / "load.csv")) >= 5):
                if worker.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Load worker failed readiness")
                time.sleep(.05)
        with (root / "benchmark.log").open("w") as log:
            subprocess.run([str(binary), "[layer_specialization_benchmark]"], env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=25)
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
    rows = h.read_csv(root / "benchmark.csv")
    if len(rows) != 96:
        raise RuntimeError("Incomplete benchmark")
    result = {"mode": mode, "gpu": h.gpu_label(root / "benchmark.log"), "groups": {}}
    if worker is not None:
        if worker.returncode != 0 or h.gpu_label(root / "load.log") != result["gpu"]:
            raise RuntimeError("Load worker failed or selected a different GPU")
        load = h.read_csv(root / "load.csv")
        overlaps = [any(int(item["host_start_ns"]) < int(row["host_end_ns"]) and
                        int(item["host_end_ns"]) > int(row["host_start_ns"]) for item in load) for row in rows]
        result["load_overlap_fraction"] = sum(overlaps) / len(overlaps)
        if mode == "saturated" and not all(overlaps):
            raise RuntimeError("Load did not cover every sample")
    for scale, quads in ((s, q) for s in ("1.00", "1.40") for q in ("1", "4")):
        values = {(row["variant"], int(row["round"])): float(row["total_ms"])
                  for row in rows if row["scale"] == scale and row["quads"] == quads}
        baseline = [values["baseline", i] for i in range(12)]
        specialized = [values["specialized", i] for i in range(12)]
        result["groups"][scale + "-" + quads] = {"baseline_ms": statistics.median(baseline),
                                   "specialized_ms": statistics.median(specialized),
                                   "paired_saving_percent": 100 * (1 - statistics.median(c / b for b, c in zip(baseline, specialized)))}
    (root / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--binary", type=Path, default=Path("build/arm64/tests/tests_render_compute"))
    p.add_argument("--modes", nargs="+", choices=("idle", "paced", "saturated"), default=("idle", "saturated"))
    a = p.parse_args()
    a.output = a.output.resolve()
    a.output.mkdir(parents=True, exist_ok=False)
    binary = a.binary.resolve()
    manifest = {"base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                                  [Path("tests/projection_quad_test_fixture.h"), Path("tests/render_compute_test_fixture.h"), Path("tests/tests_layer_specialization.cpp"), Path("tests/CMakeLists.txt"),
                                   Path("src/xrt/auxiliary/render/shaders/layer.comp"), Path("src/xrt/auxiliary/render/shaders/distortion.comp")]},
                "output": "4000x2040", "intermediate_linear_scales": [1, 1.4], "modes": a.modes,
                "workload": "projection plus one/four unpremultiplied alpha quads; bounds enabled in both; chromatic distortion and scanline timewarp; depth/foveation/chroma key inactive"}
    (a.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    results = []
    for repeat in (1, 2):
        for mode in a.modes if repeat == 1 else list(reversed(a.modes)):
            root = a.output / f"{mode}-{repeat}"
            print("Running", root.name, flush=True)
            result = capture(binary, root, mode)
            result["repeat"] = repeat
            results.append(result)
            (a.output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
            print(result["groups"], flush=True)


if __name__ == "__main__":
    main()
