#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Capture UE/PS VR2 tracking freshness, preserving the registered service.

Run on macOS with the headset connected and no XR client running. Default:
10-second warmup, then 60 seconds of gentle rotation. If Game Mode is active,
bring up Cmd+Esc for the middle third and dismiss it for the final third.
Phase labels express the requested protocol, not confirmation of Game Mode.
The foreground window is never changed by this script.

--static --seconds 10 is a plumbing/buffering preflight, not a motion test.
The script uses the same UnrealEditor -game -vr launch as the user's captures.
"""
import argparse
import csv
import json
import os
import pathlib
import plistlib
import re
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
LABEL = "org.freedesktop.monado.service"
CLIENT_TRACES = ("shared_tracking", "late_render", "presented", "frame_pipeline", "reprojection", "client_gpu",
                 "present", "present_complete", "present_scheduled", "present_worker", "drawable_prefetch", "ca_callback")
SERVER_TRACES = ("imu", "slam", "appkit_pump")
UE_LOW_COST_COMMANDS = (
    "xr.SecondaryScreenPercentage.HMDRenderTarget 25,"
    "sg.ViewDistanceQuality 0,sg.AntiAliasingQuality 0,sg.ShadowQuality 0,"
    "sg.GlobalIlluminationQuality 0,sg.ReflectionQuality 0,sg.PostProcessQuality 0,"
    "sg.EffectsQuality 0,sg.FoliageQuality 0,sg.ShadingQuality 0"
)


def host_ns():
    # os_monotonic_get_ns uses CLOCK_MONOTONIC. Python's time.monotonic_ns()
    # uses another Darwin clock on some versions; do not join it to Monado CSVs.
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def pids(name):
    result = subprocess.run(["pgrep", "-x", name], capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise RuntimeError(f"pgrep {name}: {result.stderr}")
    return [int(x) for x in result.stdout.split()]


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def bootstrap(domain, plist):
    # launchd can briefly retain the registration after bootout.
    for attempt in range(12):
        result = subprocess.run(["launchctl", "bootstrap", domain, str(plist)], capture_output=True, text=True)
        if result.returncode == 0:
            return
        if attempt == 11:
            raise RuntimeError(f"Could not restore/register {plist}: {result.stderr}")
        wait(min(2, .2 * (attempt + 1)))


def wait(seconds):
    # Keep individual sleeps short so Ctrl+C remains responsive.
    end = time.monotonic() + seconds
    while True:
        remaining = end - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(.25, remaining))


def flush_capture(out, client, server, record="post-window-flush.json"):
    requested_ns = host_ns()
    # Measurement has ended. Wait for both runtimes to acknowledge fflush(NULL)
    # before stopping UE, whose signal exit can bypass XR/stdio teardown.
    requests = []
    for side, pid in (("client", client), ("server", server)):
        request = out / side / f"monado_trace_{pid}.flush-request"
        ack = out / side / f"monado_trace_{pid}.flush-complete"
        if request.exists():
            raise RuntimeError(f"Stale flush request: {request}")
        if ack.exists():
            ack.unlink()
        request.write_text("flush after measurement\n")
        request.chmod(0o600)
        requests.append(ack)
    deadline = time.monotonic() + 15
    while not all(path.exists() for path in requests):
        if time.monotonic() >= deadline:
            raise RuntimeError("Post-window flush was not acknowledged; capture is incomplete")
        wait(.1)
    (out / record).write_text(json.dumps({"requested_ns": requested_ns, "acknowledged_ns": host_ns(),
                                                          "acknowledgements": [str(p) for p in requests]}, indent=2))
    return requested_ns


def wait_service_exit(pid, out):
    # Client shutdown triggers the capture service's idle-exit policy. Normal
    # driver/compositor teardown closes the buffers after measurement ends.
    deadline = time.monotonic() + 15
    exited = True
    while pid in pids("monado-service"):
        if time.monotonic() >= deadline:
            exited = False
            print("Service idle exit is slow; the capture registration will be stopped during restoration", flush=True)
            break
        wait(.25)
    (out / "shutdown-check.json").write_text(json.dumps({"service_pid": pid, "exited_ns": host_ns(),
                                                       "idle_exit_observed": exited, "method": "UE interrupt; acknowledged flush before teardown"}, indent=2))


def stop_ue(pid, launcher, diagnostics=None):
    if pid and pid in pids("UnrealEditor"):
        os.kill(pid, signal.SIGINT)
        # A second interrupt can force UE to exit before XR teardown closes
        # fully buffered CSVs. Allow normal cleanup before sending it.
        deadline = time.monotonic() + 15
        sampled = False
        while pid in pids("UnrealEditor") and time.monotonic() < deadline:
            if diagnostics is not None and not sampled and time.monotonic() > deadline - 12:
                sampled = True
                with (diagnostics / "ue-shutdown-sample-command.log").open("w") as log:
                    subprocess.run(["sample", str(pid), "1", "1", "-file", str(diagnostics / "ue-shutdown-sample.txt")],
                                   stdout=log, stderr=log, timeout=5)
            wait(.25)
        if pid in pids("UnrealEditor"):
            os.kill(pid, signal.SIGINT)
    if launcher:
        launcher.wait(timeout=90)


def required_paths(out, client, server):
    return ([out / "client" / f"monado_psvr2_{client}_{name}.csv" for name in CLIENT_TRACES] +
            [out / "server" / f"monado_psvr2_{server}_{name}.csv" for name in SERVER_TRACES])


def check_buffered(paths, baseline=None):
    sizes = {}
    for path in paths:
        if not path.exists():
            raise RuntimeError(f"Required trace was not opened: {path}")
        sizes[str(path)] = path.stat().st_size
        expected = baseline[str(path)] if baseline is not None else 0
        if sizes[str(path)] != expected:
            raise RuntimeError(f"Trace changed before post-window flush: {path} ({sizes[str(path)]} vs {expected} bytes)")
    return sizes


def check_sensor_health(out, server, reference_ns):
    health = {}
    inactive = []
    for name, column in (("imu", "host_callback_ns"), ("slam", "host_received_ns")):
        path = out / "server" / f"monado_psvr2_{server}_{name}.csv"
        with path.open(newline="") as f:
            count, latest = 0, 0
            for row in csv.DictReader(f):
                count += 1
                latest = max(latest, int(row[column]))
        active = latest != 0 and latest >= reference_ns - 250_000_000
        health[name] = {"rows": count, "active": active, "latest_receipt_ns": latest,
                        "age_at_request_ms": (reference_ns - latest) / 1e6 if latest else None}
        if not active:
            inactive.append(name.upper())
    (out / "sensor-health.json").write_text(json.dumps(health, indent=2))
    if inactive:
        raise RuntimeError(f"Headset {' and '.join(inactive)} stream inactive/stale after warmup; "
                           "no measurement started. UE was stopped by the capture health check. "
                           "Check headset power and USB connection before retrying. "
                           f"Details: {out / 'sensor-health.json'}")


def policy_sample(probe, out, client, server):
    sample = {"host_ns": host_ns(), "processes": {}}
    for name, pid in (("client", client), ("server", server)):
        result = subprocess.run([str(probe), "--role", "query", "--pid", str(pid)],
                                capture_output=True, text=True, timeout=5)
        sample["processes"][name] = {"pid": pid, "returncode": result.returncode,
                                     "policy": result.stdout.strip(), "error": result.stderr.strip()}
    with (out / "policy.jsonl").open("a") as f:
        f.write(json.dumps(sample) + "\n")


def capture(args):
    if sys.platform != "darwin":
        raise RuntimeError("This capture requires macOS")
    build = args.build.resolve()
    cache = (build / "CMakeCache.txt").read_text()
    if f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}" not in cache:
        raise RuntimeError("Build directory belongs to another checkout")
    for name in ("UnrealEditor", "monado-service"):
        if pids(name):
            raise RuntimeError(f"Close existing {name} processes first; no process was stopped")
    domain = f"gui/{os.getuid()}"
    target = f"{domain}/{LABEL}"
    registration = run(["launchctl", "print", target], capture_output=True, text=True).stdout
    match = re.search(r"^\s*path = (.+)$", registration, re.M)
    if not match:
        raise RuntimeError("Cannot identify the original service registration")
    original = pathlib.Path(match.group(1))
    config = plistlib.loads(original.read_bytes())
    service = build / "src/xrt/targets/service/monado-service"
    if pathlib.Path(config["ProgramArguments"][0]).resolve() != service.resolve():
        raise RuntimeError("Registered service is from another build; refusing to replace it")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "client").mkdir()
    (out / "server").mkdir()
    (out / "original-service.plist").write_bytes(original.read_bytes())
    (out / "original-registration.txt").write_text(registration)
    env = dict(config.get("EnvironmentVariables", {}))
    env.update(PSVR2_TIMING_TRACE="1", PSVR2_DRIVER_TIMING_TRACE="1",
               PSVR2_TIMING_TRACE_FULLY_BUFFERED="1", PSVR2_TIMING_TRACE_DIR=str(out / "server"),
               IPC_EXIT_WHEN_IDLE="1", IPC_EXIT_WHEN_IDLE_DELAY_MS="1000")
    config["EnvironmentVariables"] = env
    config["StandardOutPath"] = str(out / "server.log")
    config["StandardErrorPath"] = str(out / "server.log")
    temporary = out / "capture-service.plist"
    temporary.write_bytes(plistlib.dumps(config))
    client_env = dict(IPC_LOG="info", XRT_MACOS_CLIENT_COMPOSITOR="1",
                      XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD="1", XRT_MACOS_DISPLAY_LINK="ca",
                      XRT_MACOS_SHARED_TRACKING="1", XR_RUNTIME_JSON=str(build / "openxr_monado-dev.json"),
                      UE_OPENXR_LOADER_LIBRARY="/usr/local/lib/libopenxr_loader.dylib",
                      PSVR2_TIMING_TRACE="1", PSVR2_TIMING_TRACE_FULLY_BUFFERED="1",
                      PSVR2_TIMING_TRACE_DIR=str(out / "client"))
    argv = ["open", "-n", "-W", "--stdout", str(out / "client.log"), "--stderr", str(out / "client.log")]
    for key, value in client_env.items():
        argv += ["--env", f"{key}={value}"]
    argv += [str(args.editor), "--args", str(args.project), "-game", "-vr", "-NOSCREENMESSAGES",
             "-LogCmds=LogHMD VeryVerbose", "-stdout", "-log"]
    exec_commands = UE_LOW_COST_COMMANDS if args.ue_preset == "low-cost" else None
    if exec_commands:
        # One argv element; quotes belong to shell syntax, not the argument.
        argv.append(f"-ExecCmds={exec_commands}")
    metadata = dict(branch=run(["git", "branch", "--show-current"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
                    head=run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
                    uncommitted=True, motion="static" if args.static else "rotational",
                    seconds=args.seconds, warmup_seconds=10, pre_measurement_settle_seconds=2,
                    sensor_health_gate=True, game_mode="not independently confirmed",
                    game_mode_protocol=args.game_mode_protocol,
                    ue_preset=args.ue_preset, ue_exec_commands=exec_commands,
                    client_env=client_env, service_env=env, argv=argv, original_service=str(original))
    (out / "worktree.patch").write_text(run(["git", "diff"], cwd=ROOT, capture_output=True, text=True).stdout)
    files = run(["git", "ls-files", "--others", "--exclude-standard"], cwd=ROOT, capture_output=True).stdout
    run(["tar", "-czf", str(out / "new-files.tar.gz"), "-T", "-"], cwd=ROOT, input=files)
    changed = False
    launcher = None
    client = server = None
    try:
        run(["launchctl", "bootout", target])
        changed = True
        bootstrap(domain, temporary)
        print(f"Launching UE with the existing game/VR arguments; preset={args.ue_preset}", flush=True)
        with (out / "launcher.log").open("w") as log:
            launcher = subprocess.Popen(argv, stdout=log, stderr=log)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if launcher.poll() is not None:
                raise RuntimeError("UE exited during startup")
            log_path = out / "client.log"
            if log_path.exists() and "hosted front-end visible=true" in log_path.read_text():
                break
            wait(.5)
        else:
            raise RuntimeError("UE startup timed out")
        clients, servers = pids("UnrealEditor"), pids("monado-service")
        if len(clients) != 1 or len(servers) != 1:
            raise RuntimeError("Cannot identify the two capture processes uniquely")
        client, server = clients[0], servers[0]
        metadata.update(client_pid=client, server_pid=server)
        (out / "process.json").write_text(json.dumps(metadata, indent=2))
        paths = required_paths(out, client, server)
        wait(10)
        check_buffered(paths)
        # Flush only outside measurement to inspect source health. Finite
        # buffers start the window fresh; fixed file sizes prove no later writes.
        reference_ns = flush_capture(out, client, server, "warmup-flush.json")
        check_sensor_health(out, server, reference_ns)
        baseline = {str(path): path.stat().st_size for path in paths}
        (out / "buffer-baseline.json").write_text(json.dumps(baseline, indent=2))
        wait(2)
        checks = [{"host_ns": host_ns(), "sizes": check_buffered(paths, baseline)}]
        start = host_ns()
        (out / "window.json").write_text(json.dumps({"start_ns": start, "clock": "CLOCK_MONOTONIC"}, indent=2))
        print(f"CAPTURE START: {args.seconds:g} seconds; client {client}, service {server}", flush=True)
        if not args.static:
            if args.game_mode_protocol == "continuous":
                print("Rotate gently throughout. Keep Game Mode active throughout; do not open the menu.", flush=True)
            else:
                print("Rotate gently throughout. If Game Mode is active, Cmd+Esc in the middle third; dismiss for the last third.", flush=True)
        probe = build / "src/xrt/targets/macos_layer_host_probe/macos-layer-host-probe"
        next_policy = next_check = 0
        phase = -1
        while (host_ns() - start) / 1e9 < args.seconds:
            elapsed = (host_ns() - start) / 1e9
            now_phase = min(2, int(3 * elapsed / args.seconds))
            if now_phase != phase:
                phase = now_phase
                print(f"Phase {phase + 1}/3 at {elapsed:.1f}s", flush=True)
                if args.announce:
                    cues = (["Capture started. Keep Game Mode on", "Middle third. Keep Game Mode on",
                             "Final third. Keep Game Mode on"] if args.game_mode_protocol == "continuous" else
                            ["Capture started", "Middle third. Open the menu", "Final third. Close the menu"])
                    subprocess.Popen(["say", cues[phase]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if elapsed >= next_policy:
                policy_sample(probe, out, client, server)
                next_policy = elapsed + 2
            if elapsed >= next_check:
                checks.append({"host_ns": host_ns(), "sizes": check_buffered(paths, baseline)})
                next_check = elapsed + 5
            wait(.2)
        end = host_ns()
        checks.append({"host_ns": end, "sizes": check_buffered(paths, baseline)})
        (out / "buffer-checks.json").write_text(json.dumps(checks, indent=2))
        (out / "window.json").write_text(json.dumps({"start_ns": start, "end_ns": end, "clock": "CLOCK_MONOTONIC"}, indent=2))
        print("MEASUREMENT COMPLETE; requesting buffered trace flush", flush=True)
        wait(.1)
        flush_capture(out, client, server)
        stop_ue(client, launcher, out)
        client = None
        launcher = None
        wait_service_exit(server, out)
        counts = {}
        for path in paths:
            with path.open(newline="") as f:
                count = sum(1 for _ in csv.DictReader(f))
            if count == 0:
                raise RuntimeError(f"Empty flushed trace: {path}")
            counts[str(path)] = count
        (out / "flush-check.json").write_text(json.dumps(counts, indent=2))
        print(f"COMPLETE: {out}", flush=True)
    finally:
        try:
            # The launched app belongs to this capture; existing apps were rejected.
            if launcher:
                if client is None:
                    candidates = pids("UnrealEditor")
                    client = candidates[0] if len(candidates) == 1 else None
                stop_ue(client, launcher)
        finally:
            if changed:
                subprocess.run(["launchctl", "bootout", target], check=False)
                bootstrap(domain, original)
                (out / "service-restored.txt").write_text(f"Restored {original}\n")
                print("Original service registration restored", flush=True)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=pathlib.Path, help="new capture directory (must not exist)")
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / "build-wine")
    parser.add_argument("--editor", type=pathlib.Path, default=pathlib.Path.home() / "Code/UnrealEngine/Engine/Binaries/Mac/UnrealEditor.app")
    parser.add_argument("--project", type=pathlib.Path, default=pathlib.Path.home() / "Documents/Unreal Projects/MonadoMacTest/MonadoMacTest.uproject")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--static", action="store_true")
    parser.add_argument("--ue-preset", choices=("standard", "low-cost"), default="standard",
                        help="low-cost applies the user's 25%% HMD render-target and quality-zero ExecCmds preset")
    parser.add_argument("--game-mode-protocol", choices=("menu", "continuous"), default="menu",
                        help="continuous keeps Game Mode on for the whole workload comparison")
    parser.add_argument("--announce", action="store_true", help="speak phase cues, without changing foreground focus")
    args = parser.parse_args()
    if not 5 <= args.seconds <= 90:
        parser.error("--seconds must be between 5 and 90 to bound sensor-buffer use")
    try:
        out = capture(args)
    except RuntimeError as error:
        parser.exit(1, f"Capture stopped: {error}\n")
    with (out / "analysis.log").open("w") as log:
        run([sys.executable, str(ROOT / "scripts/macos/analyze-tracking-freshness.py"), str(out)], stdout=log)
    with (out / "presentation-analysis.log").open("w") as log:
        run([sys.executable, str(ROOT / "scripts/macos/analyze-presentation-stalls.py"), str(out)], stdout=log)
    print(f"Analysis ready: {out / 'freshness-summary.json'}", flush=True)
    print(f"Presentation joins ready: {out / 'presentation-stalls.json'}", flush=True)


if __name__ == "__main__":
    main()
