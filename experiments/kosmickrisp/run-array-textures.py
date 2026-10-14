#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0
import argparse
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import uuid

parser = argparse.ArgumentParser(description="Isolated KosmicKrisp shared-texture diagnostic")
parser.add_argument("icd", type=Path)
parser.add_argument("dylib", type=Path)
parser.add_argument("--negative-control", action="store_true", help="Omit the peer's GPU producer wait; must fail")
parser.add_argument("--format", choices=["rgba","bgra","rgba-srgb","bgra-srgb"], default="rgba")
parser.add_argument("--layers", type=int, choices=[1,2], default=2)
parser.add_argument("--include", type=Path, required=True)
args = parser.parse_args()
here = Path(__file__).resolve().parent
work = Path(tempfile.mkdtemp(prefix="kk-shared-texture-"))
print(f"evidence directory: {work}", flush=True)
name = "org.monado.kosmickrisp-texture-test." + uuid.uuid4().hex
domain = f"gui/{os.getuid()}"
brew = subprocess.check_output(["brew", "--prefix"], text=True).strip()
common = ["xcrun", "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-framework", "Metal", "-framework", "Foundation", "-framework", "IOSurface"]
subprocess.run([*common, str(here / "array-texture-peer.mm"), "-o", str(work / "peer")], check=True)
subprocess.run([*common, "-Wno-missing-field-initializers", f"-I{brew}/include", f"-I{args.include}", str(here / "array-texture-probe.mm"), f"-L{brew}/lib", "-lvulkan", f"-Wl,-rpath,{brew}/lib", "-o", str(work / "probe")], check=True)
for stage in ["vert", "frag"]:
    subprocess.run(["glslangValidator", "-V", str(here / ("pattern." + stage)), "-o", str(work / (stage + ".spv"))], check=True)
peer_env = {"MTL_DEBUG_LAYER": "1"}
if args.negative_control:
    peer_env["KK_TEST_SKIP_WAIT"] = "1"
plist = work / "peer.plist"
plist.write_bytes(plistlib.dumps({"Label": name, "ProgramArguments": [str(work / "peer"), name], "MachServices": {name: True}, "EnvironmentVariables": peer_env, "StandardOutPath": str(work / "peer.log"), "StandardErrorPath": str(work / "peer.log")}))
bootstrapped = False
try:
    subprocess.run(["launchctl", "bootstrap", domain, str(plist)], check=True, timeout=10)
    bootstrapped = True
    env = dict(os.environ, VK_DRIVER_FILES=str(args.icd.resolve()), MTL_DEBUG_LAYER="1")
    with (work / "probe.log").open("w") as log:
        result = subprocess.run([str(work / "probe"), str(args.dylib.resolve()), name, args.format, str(work / "vert.spv"), str(work / "frag.spv"), str(args.layers)], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=30)
    text = (work / "probe.log").read_text()
    for line in text.splitlines():
        if line.startswith(("cross_process", "gpu_chain", "PASS", "premature", "diagnostic")):
            print(line)
    if args.negative_control:
        if result.returncode == 1 and "premature completion" in text:
            print("PASS negative control: missing peer wait detected before gate release")
        else:
            print(text)
            raise SystemExit("FAIL negative control did not detect missing GPU wait")
    elif result.returncode:
        print(text)
        raise SystemExit(result.returncode)
except subprocess.TimeoutExpired:
    raise SystemExit("FAIL bounded test deadline expired")
finally:
    if bootstrapped:
        cleanup = subprocess.run(["launchctl", "bootout", domain, str(plist)], timeout=10)
        if cleanup.returncode:
            print(f"WARNING: test service cleanup failed: {name}")
