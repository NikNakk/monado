#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Compare opt-in compute pass timestamps with unchanged CA-copy presentation."""
import argparse
import hashlib
import importlib.util
import json
import subprocess
from pathlib import Path

spec = importlib.util.spec_from_file_location("headset", Path(__file__).with_name("run-headset-gpu-load.py"))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


def summarize_passes(root, result, enabled):
    lo, hi = result["window_start_ns"], result["window_end_ns"]
    stages = h.trace(root, "renderer_stage")
    for stage in ("previous_fence_wait", "draw_dispatch", "queue_submit", "target_acquire"):
        result[stage + "_ms"] = h.distribution([
            (int(row["end_ns"]) - int(row["begin_ns"])) / 1e6 for row in stages
            if row["stage"] == stage and lo <= int(row["begin_ns"]) <= hi
        ])
    passes = [row for row in h.trace(root, "compositor_gpu_pass")
              if lo <= int(row["feedback_ns"]) <= hi] if enabled else []
    if not enabled:
        if list(root.glob("*_compositor_gpu_pass.csv")):
            raise RuntimeError("Pass queries unexpectedly active in control")
        return result
    if len(passes) < 120:
        raise RuntimeError("Insufficient fence-completed pass samples")
    totals = {row["frame_id"]: int(row["gpu_duration_ns"])
              for row in h.trace(root, "compositor_gpu")}
    if any(row["path"] != "2" for row in passes):
        raise RuntimeError("Expected layer-composition workload throughout steady state")
    if any(not 0 < int(row["composition_ns"]) + int(row["distortion_ns"])
           <= totals[row["frame_id"]] + 10 for row in passes):
        raise RuntimeError("Pass brackets fail whole-frame consistency")
    result["pass_frames"] = len(passes)
    for field in ("composition_ns", "distortion_ns"):
        result[field.replace("_ns", "_ms")] = h.distribution([int(row[field]) / 1e6 for row in passes])
    result["outside_passes_ms"] = h.distribution([
        (totals[row["frame_id"]] - int(row["composition_ns"]) - int(row["distortion_ns"])) / 1e6
        for row in passes
    ])
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--build", type=Path, default=Path("build/arm64"))
    p.add_argument("--seconds", type=int, choices=range(5, 91), default=15)
    p.add_argument("--repeats", type=int, choices=(1, 2), default=2)
    p.add_argument("--modes", nargs="+", choices=("idle", "paced", "saturated"),
                   default=("idle", "paced", "saturated"))
    p.add_argument("--timing", nargs="+", type=int, choices=(0, 1), default=(0, 1))
    a = p.parse_args()
    a.refresh_hz = 120
    a.build = a.build.resolve()
    a.output = a.output.resolve()
    a.output.mkdir(parents=True, exist_ok=False)
    files = ["src/xrt/auxiliary/render/" + name for name in
             ("render_resources.c", "render_compute.c", "render_interface.h")]
    files += ["src/xrt/compositor/main/comp_renderer.c", "src/xrt/compositor/util/comp_render_cs.c"]
    manifest = {"base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "service_sha256": hashlib.sha256((a.build / "src/xrt/targets/service/monado-service").read_bytes()).hexdigest(),
                "source_sha256": {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in files},
                "seconds": a.seconds, "repeats": a.repeats, "modes": a.modes, "timing": a.timing,
                "workload": "four quads, bounds enabled, normal render scale, CA copy"}
    (a.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    results = []
    for repeat in range(1, a.repeats + 1):
        for mode in a.modes if repeat == 1 else list(reversed(a.modes)):
            for timing in a.timing if repeat == 1 else list(reversed(a.timing)):
                root = a.output / f"{mode}-{repeat}-timing-{timing}"
                print("Running", root.name, flush=True)
                settings = {"XRT_COMPOSITOR_COMPUTE": "1", "XRT_MACOS_COMPOSITOR_QUAD_BOUNDS": "1",
                            "XRT_MACOS_GPU_PASS_TIMING": str(timing)}
                result = h.capture(a, root, mode, settings, "quads-four")
                result["pass_timing_enabled"] = bool(timing)
                summarize_passes(root, result, timing)
                results.append(result)
                for path, data in ((root / "presentation-summary.json", result),
                                   (a.output / "summary.json", results)):
                    path.write_text(json.dumps(data, indent=2) + "\n")
                print(f"{result['physical_hz']:.2f} Hz; fence {result['previous_fence_wait_ms']['p50']} ms; "
                      f"composition {result.get('composition_ms', {}).get('p50')} ms; "
                      f"distortion {result.get('distortion_ms', {}).get('p50')} ms", flush=True)


if __name__ == "__main__":
    main()
