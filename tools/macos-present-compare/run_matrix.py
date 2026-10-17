#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Run captures serially, rotating order between repeats; preserve commands and logs."""
import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--display-name", default="PS VR2")
    parser.add_argument("--frames", type=int, default=2700)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--burn", type=int, default=0)
    parser.add_argument("--delay-ms", type=float, default=0)
    parser.add_argument("--delay-every", type=int, default=0)
    parser.add_argument("--policy", choices=("immediate", "timed"), default="immediate")
    parser.add_argument("--native-minimum", action="store_true", help="native uses minimum duration; WSI keeps --policy")
    parser.add_argument("--windowed", action="store_true")
    args = parser.parse_args()
    if args.frames < 1 or args.repeats < 1:
        parser.error("frames and repeats must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = ROOT / ".build/present-compare"
    files = [ROOT / n for n in ("main.mm", "shader.vert", "shader.frag", "build.sh", "run_matrix.py", "analyze.py")]
    manifest = {"start_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "sha256": {str(p.relative_to(ROOT)): sha(p) for p in files + [binary]},
                "runs": []}
    manifest_path = output / "manifest.json"
    try:
        modes = ["native", "wsi-copy", "wsi-direct"]
        for repeat in range(args.repeats):
            order = modes[repeat % 3:] + modes[:repeat % 3]
            for mode in order:
                if sha(binary) != manifest["sha256"][str(binary.relative_to(ROOT))]:
                    raise RuntimeError("Binary changed during capture matrix")
                trace = output / f"r{repeat + 1}-{mode}.csv"
                policy = "minimum" if mode == "native" and args.native_minimum else args.policy
                command = [str(binary), "--mode", mode, "--policy", policy,
                           "--display-name", args.display_name, "--frames", str(args.frames),
                           "--burn", str(args.burn), "--shader-dir", str(ROOT / ".build"), "--trace", str(trace)]
                if args.delay_every or args.delay_ms:
                    command += ["--delay-ms", str(args.delay_ms), "--delay-every", str(args.delay_every)]
                if args.windowed:
                    command += ["--windowed"]
                run = {"command": command, "start_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
                manifest["runs"].append(run)
                print(f"Repeat {repeat + 1}/{args.repeats}: {mode}/{policy}", flush=True)
                with trace.with_suffix(".log").open("w") as log:
                    process = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                             timeout=max(60, args.frames / 15 + 30))
                run["exit_code"] = process.returncode
                run["end_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
                if process.returncode:
                    raise RuntimeError(f"Capture failed; see {trace.with_suffix('.log')}")
                manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    finally:
        manifest["end_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
