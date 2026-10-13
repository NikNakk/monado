#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Compare opt-in common layers using projection plus four quads and CA copying."""
import argparse
import hashlib
import importlib.util
import json
import subprocess
from pathlib import Path

spec = importlib.util.spec_from_file_location("headset", Path(__file__).with_name("run-headset-gpu-load.py"))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--build", type=Path, default=Path("build/arm64"))
    p.add_argument("--seconds", type=int, choices=range(5, 91), default=15)
    p.add_argument("--repeats", type=int, choices=(1, 2), default=2)
    p.add_argument("--modes", nargs="+", choices=("idle", "paced", "saturated"), default=("idle", "saturated"))
    p.add_argument("--common", nargs="+", type=int, choices=(0, 1), default=(0, 1))
    a = p.parse_args()
    a.refresh_hz = 120
    a.build = a.build.resolve()
    a.output = a.output.resolve()
    a.output.mkdir(parents=True, exist_ok=False)
    files = [Path(name) for name in (
        "src/xrt/auxiliary/render/shaders/layer.comp", "src/xrt/auxiliary/render/render_layer_specialization.h",
        "src/xrt/auxiliary/render/render_compute.c", "src/xrt/auxiliary/render/render_resources.c",
        "src/xrt/compositor/util/comp_render_cs.c", "tests/tests_macos_service_gpu_sync.mm")]
    binaries = [a.build / "src/xrt/targets/service/monado-service", a.build / "tests/tests_macos_service_gpu_sync",
                a.build / "tests/tests_render_compute"]
    (a.output / "manifest.json").write_text(json.dumps({
        "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        "binary_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in binaries},
        "seconds": a.seconds, "repeats": a.repeats, "modes": a.modes, "common": a.common,
        "workload": "stereo projection plus four opaque head-locked quads from a uniform 64x64 Metal source; bounds enabled; normal runtime scale; CA copy; pass queries off"
    }, indent=2) + "\n")
    results = []
    for repeat in range(1, a.repeats + 1):
        for mode in a.modes if repeat == 1 else reversed(a.modes):
            for common in a.common if repeat == 1 else reversed(a.common):
                root = a.output / f"{mode}-{repeat}-common-{common}"
                print("Running", root.name, flush=True)
                settings = {"XRT_COMPOSITOR_COMPUTE": "1", "XRT_MACOS_COMPOSITOR_QUAD_BOUNDS": "1",
                            "XRT_MACOS_COMPOSITOR_COMMON_LAYERS": str(common), "XRT_MACOS_GPU_PASS_TIMING": "0",
                            "XRT_COMPOSITOR_LOG": "info"}
                result = h.capture(a, root, mode, settings, "projection-quads-four")
                log = (root / "service.log").read_text()
                selected = "First common-layer compute dispatch selected" in log
                if selected != bool(common):
                    raise RuntimeError("Unexpected common-layer selection evidence")
                result.update(common_layers_enabled=bool(common), common_dispatch_observed=selected)
                results.append(result)
                for path, data in ((root / "presentation-summary.json", result), (a.output / "summary.json", results)):
                    path.write_text(json.dumps(data, indent=2) + "\n")
                print(f"{result['physical_hz']:.2f} Hz; Vulkan {result['vulkan_all_feedback_ms']['p50']:.3f} ms", flush=True)


if __name__ == "__main__":
    main()
