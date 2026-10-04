#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Prepare an isolated optimised Sense test LaunchAgent; print commands, never start hardware."""
import argparse
import json
import os
import pathlib
import plistlib
import re
import shlex

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / "build-macos-sense-rel")
    args = parser.parse_args()
    build = args.build.resolve()
    calibration = args.calibration.resolve(strict=True)
    cache = (build / "CMakeCache.txt").read_text()
    if f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\n" not in cache:
        parser.error("build directory belongs to another checkout")
    if not re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$", cache, re.M):
        parser.error("6DoF requires an optimised build; use the macos-sense-relwithdebinfo preset")
    # Check effective flags as well as the cache, including the compositor.
    commands = json.loads((build / "compile_commands.json").read_text())
    for suffix in ("joint_pose_solver.cpp", "pssense_driver.c", "comp_renderer.c", "comp_window_macos.m"):
        matches = [c for c in commands if c["file"].endswith("/" + suffix)]
        if not matches or any(not re.search(r"(?:^| )-O(?:[123sz]|fast)(?: |$)", c["command"])
                              or re.search(r"(?:^| )-O0(?: |$)", c["command"]) for c in matches):
            parser.error(f"missing or unoptimised compile command for {suffix}")
    service = build / "src/xrt/targets/service/monado-service"
    client = build / "src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test"
    manifest = build / "openxr_monado-dev.json"
    for file in (service, client, manifest):
        if not file.is_file():
            parser.error(f"build first: missing {file}")
    runtime = build / "sense-runtime"
    runtime.mkdir(mode=0o700, exist_ok=True)
    if len(str(runtime / "monado_comp_ipc").encode()) >= 104:
        parser.error("build path is too long for a macOS Unix socket")
    label = "org.freedesktop.monado.sense-integration"
    endpoint = "org.freedesktop.monado.metal-ipc.sense-integration"
    env = {k: v for k, v in os.environ.items()
           if k.startswith(("PSVR2_", "PSSENSE_", "CONSTELLATION_", "XRT_MACOS_", "XRT_COMPOSITOR_", "VK_"))}
    env.update(XRT_NO_STDIN="1", IPC_EXIT_WHEN_IDLE="1", IPC_EXIT_WHEN_IDLE_DELAY_MS="5000",
               PSVR2_SENSE_6DOF="1", PSVR2_SENSE_6DOF_CALIBRATION=str(calibration),
               XDG_RUNTIME_DIR=str(runtime), XRT_MACOS_METAL_IPC_SERVICE_NAME=endpoint)
    plist = build / "pssense-6dof.plist"
    plist.write_bytes(plistlib.dumps(dict(
        Label=label, ProgramArguments=[str(service)], WorkingDirectory=str(ROOT),
        RunAtLoad=False, KeepAlive=False, ProcessType="Interactive",
        MachServices={endpoint: True}, EnvironmentVariables=env,
        StandardOutPath=str(build / "sense-service.log"), StandardErrorPath=str(build / "sense-service.log"))))
    print("Prepared", plist)
    print("Close other PS VR2 tools and unload the existing Monado LaunchAgent before loading this one.")
    print("These commands start hardware; preparation itself has not started it:")
    print(shlex.join(["launchctl", "bootstrap", f"gui/{os.getuid()}", str(plist)]))
    client_env = dict(XDG_RUNTIME_DIR=str(runtime), XRT_MACOS_METAL_IPC_SERVICE_NAME=endpoint,
                      XR_RUNTIME_JSON=str(manifest), XRT_MACOS_CLIENT_COMPOSITOR="1")
    print(shlex.join(["env"] + [f"{k}={v}" for k, v in client_env.items()] + [str(client), "--generic-controller"]))
    print("After closing the app:")
    print(shlex.join(["launchctl", "bootout", f"gui/{os.getuid()}/{label}"]))
    print("Then restore your previous Monado LaunchAgent. Logs:", build / "sense-service.log")


if __name__ == "__main__":
    main()
