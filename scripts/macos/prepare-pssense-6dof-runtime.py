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
import hashlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / "build/arm64")
    parser.add_argument("--pose-diag", action="store_true", help="trace application pose validity and filter failures")
    parser.add_argument("--imu-fallback-ms", type=int, default=None,
                        help="application position validity after optical loss (0=default, maximum 1000)")
    parser.add_argument("--controller-survey", type=pathlib.Path, metavar="NEW-DIRECTORY",
                        help="prepare a guided controller survey with matched service CTD and pose capture")
    parser.add_argument("--controller-survey-hand", choices=("both", "left", "right"), default="both",
                        help="capture only one hand or both (default both)")
    parser.add_argument("--passthrough", action="store_true", help="show cameras behind the controller survey")
    args = parser.parse_args()
    if not args.controller_survey and (args.passthrough or args.controller_survey_hand != "both"):
        parser.error("--passthrough and --controller-survey-hand require --controller-survey")
    if args.imu_fallback_ms is not None and not 0 <= args.imu_fallback_ms <= 1000:
        parser.error("--imu-fallback-ms must be between 0 and 1000")
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
    if args.pose_diag:
        env["PSSENSE_POSE_DIAG"] = "1"
    if args.imu_fallback_ms is not None:
        env["PSSENSE_IMU_FALLBACK_MS"] = str(args.imu_fallback_ms)
    survey = None
    if args.controller_survey:
        survey = args.controller_survey.resolve()
        try:
            survey.mkdir(mode=0o700, parents=True)  # Exclusive: never overwrite a previous dataset.
        except OSError as error:
            parser.error(f"cannot create a new survey directory: {error}")
        shutil.copyfile(calibration, survey / "calibration.json")
        env["CONSTELLATION_TRACKER_DATA_RECORDER_OUTPUT"] = str(survey / "constellation.ctd")
        token = hashlib.sha256(str(survey).encode()).hexdigest()[:10]
        label = f"org.freedesktop.monado.sense-survey.{token}"
        endpoint = f"org.freedesktop.monado.metal-ipc.survey.{token}"
        runtime = build / "sense-runtime" / token
        runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
        if len(str(runtime / "monado_comp_ipc").encode()) >= 104:
            parser.error("survey runtime path is too long for a macOS Unix socket")
        env["XDG_RUNTIME_DIR"] = str(runtime)
        env["XRT_MACOS_METAL_IPC_SERVICE_NAME"] = endpoint
        # Match the user's saved floor preferences without inventing an eye height.
        saved_agent = pathlib.Path.home() / "Library/LaunchAgents/org.freedesktop.monado.service.plist"
        if saved_agent.is_file():
            saved = plistlib.loads(saved_agent.read_bytes()).get("EnvironmentVariables", {})
            for key in ("XRT_FLOOR_EYE_HEIGHT_M", "XRT_FLOOR_ALIGN"):
                if key in saved:
                    env[key] = saved[key]
        for key in ("XRT_FLOOR_EYE_HEIGHT_M", "XRT_FLOOR_ALIGN"):
            if key in os.environ:
                env[key] = os.environ[key]
        env["XRT_PSSENSE_STAGE_RECENTER"] = "1"
        env["XRT_PSSENSE_STAGE_RECENTER_HOLD_MS"] = "1000"
        env["PSSENSE_POSE_DIAG"] = "1"
        env["PSSENSE_SYNTHETIC_POSITION"] = "0"
        env["PSSENSE_SYNTHETIC_ARM_MODEL"] = "0"
        # Start with the ordinary horizon unless explicitly overridden for an A/B.
        env["PSSENSE_IMU_FALLBACK_MS"] = str(args.imu_fallback_ms or 0)
        if args.passthrough:
            env["PSVR2_CAMERA_STREAMS"] = "1"
        runtime_library = pathlib.Path(json.loads(manifest.read_text())["runtime"]["library_path"])
        if not runtime_library.is_absolute():
            runtime_library = (manifest.parent / runtime_library).resolve()
        identity = dict(schema=1, protocol="controller-survey-v3", survey_hand=args.controller_survey_hand,
                        passthrough=args.passthrough, service=str(service), client=str(client), calibration=str(calibration),
                        runtime=str(runtime_library),
                        runtime_sha256=hashlib.sha256(runtime_library.read_bytes()).hexdigest(),
                        service_sha256=hashlib.sha256(service.read_bytes()).hexdigest(),
                        client_sha256=hashlib.sha256(client.read_bytes()).hexdigest(),
                        source_head=subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
                        source_status=subprocess.check_output(["git", "-C", str(ROOT), "status", "--short"], text=True),
                        environment=env)
        (survey / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    plist = survey / "service.plist" if survey else build / "pssense-6dof.plist"
    service_log = survey / "service.log" if survey else build / "sense-service.log"
    plist.write_bytes(plistlib.dumps(dict(
        Label=label, ProgramArguments=[str(service)], WorkingDirectory=str(ROOT),
        RunAtLoad=False, KeepAlive=False, ProcessType="Interactive",
        MachServices={endpoint: True}, EnvironmentVariables=env,
        StandardOutPath=str(service_log), StandardErrorPath=str(service_log))))
    print("Prepared", plist)
    print("Close other PS VR2 tools and unload the existing Monado LaunchAgent before loading this one.")
    print("These commands start hardware; preparation itself has not started it:")
    print(shlex.join(["launchctl", "bootstrap", f"gui/{os.getuid()}", str(plist)]))
    client_env = dict(XDG_RUNTIME_DIR=str(runtime), XRT_MACOS_METAL_IPC_SERVICE_NAME=endpoint,
                      XR_RUNTIME_JSON=str(manifest), XRT_MACOS_CLIENT_COMPOSITOR="1")
    client_args = [str(client), "--controller-survey", str(survey / "guided")] if survey else [str(client), "--generic-controller"]
    if survey:
        client_args += ["--controller-survey-hand", args.controller_survey_hand]
        if args.passthrough:
            client_args += ["--passthrough"]
    client_command = shlex.join(["env"] + [f"{k}={v}" for k, v in client_env.items()] + client_args)
    if survey:
        client_command += " 2>&1 | tee " + shlex.quote(str(survey / "client.log"))
    print(client_command)
    print("After closing the app:")
    print(shlex.join(["launchctl", "bootout", f"gui/{os.getuid()}/{label}"]))
    print("Then restore your previous Monado LaunchAgent. Logs:", service_log)


if __name__ == "__main__":
    main()
