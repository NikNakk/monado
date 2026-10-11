#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Run an isolated GPU service test, offscreen or on the physical headset."""
import argparse
import csv
import re
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build/arm64'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--client', nargs='+')
    parser.add_argument('--benchmark', action='store_true', help='Disable readback and per-image diagnostic logging')
    parser.add_argument('--headset', action='store_true', help='Use the physical headset and display; requires exclusive USB ownership')
    parser.add_argument('--unique-output', action='store_true')
    parser.add_argument('--fault', choices=['missing', 'disconnect'], default='missing')
    parser.add_argument('--suite', action='store_true', help='Test a healthy client, failed overlay and replacement client in one service')
    args = parser.parse_args()
    if args.suite and (args.client or args.benchmark):
        parser.error('--suite cannot be combined with --client or --benchmark')
    build = args.build.resolve()
    label = f'org.freedesktop.monado.gpu-sync-service.{os.getpid()}'
    if args.unique_output:
        args.output /= label
    args.output.mkdir(parents=True, exist_ok=True)
    out = args.output.resolve()
    print(f'GPU service results: {out}', flush=True)
    domain = f'gui/{os.getuid()}'
    with tempfile.TemporaryDirectory(prefix='monado-gpu-service-') as temporary:
        env = {k: v for k, v in os.environ.items() if not k.startswith(('XRT_', 'PSVR2_', 'SIMULATED_', 'OXR_'))}
        env.update(XDG_RUNTIME_DIR=temporary, XDG_CONFIG_HOME=temporary,
                   XRT_NO_STDIN='1',
                   XRT_LOG='info', XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE='1',
                   PSVR2_TIMING_TRACE='1', PSVR2_TIMING_TRACE_FULLY_BUFFERED='1',
                   PSVR2_TIMING_TRACE_DIR=str(out),
                   XRT_COMPOSITOR_LOG_APPLE_SAMPLES='1',
                   SIMULATED_ENABLE='1', XRT_COMPOSITOR_NULL='0',
                   XRT_COMPOSITOR_FORCE_DEBUG_IMAGE='1',
                   XRT_MACOS_CLIENT_COMPOSITOR='0',
                   XRT_MACOS_METAL_IPC_SERVICE_NAME=label,
                   MONADO_OPENXR_RUNTIME_PATH=str(build/'src/xrt/targets/openxr/libopenxr_monado.dylib'))
        if args.benchmark:
            env.update(XRT_LOG='warn', XRT_COMPOSITOR_LOG_APPLE_SAMPLES='0',
                       XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE='0')
        if args.headset:
            env.update(SIMULATED_ENABLE='0', XRT_COMPOSITOR_FORCE_DEBUG_IMAGE='0',
                       XRT_MACOS_PREVENT_DISPLAY_SLEEP='1', PSVR2_CAMERA_STREAMS='0')
        telemetry = {'start_epoch': time.time(), 'workload': ('64x64 Metal quad; physical PS VR2 display' if args.headset else '64x64 Metal quad; simulated HMD; debug_image target'),
                     'benchmark': args.benchmark, 'powermetrics': 'not collected; power/frequency unavailable',
                     'game_mode': 'not requested'}
        telemetry['headset'] = args.headset
        for key, command in [('power', ['pmset', '-g', 'batt']), ('settings', ['pmset', '-g', 'custom']),
                             ('os', ['sw_vers']), ('background_cpu', ['ps', '-A', '-o', '%cpu,comm'])]:
            snapshot = subprocess.run(command, capture_output=True, text=True, timeout=5)
            telemetry[key] = {'exit': snapshot.returncode, 'stdout': snapshot.stdout}
        (out/'telemetry.json').write_text(json.dumps(telemetry, indent=2)+'\n')
        plist = Path(temporary)/'service.plist'
        plist.write_bytes(plistlib.dumps(dict(Label=label,
            ProgramArguments=[str(build/'src/xrt/targets/service/monado-service')],
            EnvironmentVariables=env, MachServices={label: True}, RunAtLoad=True,
            StandardOutPath=str(out/'service.log'), StandardErrorPath=str(out/'service.log'))))
        subprocess.run(['launchctl', 'bootstrap', domain, str(plist)], check=True, timeout=10)
        try:
            deadline = time.monotonic()+15
            while not (Path(temporary)/'monado_comp_ipc').exists():
                if time.monotonic() >= deadline:
                    raise RuntimeError('Service socket did not become ready; inspect service.log')
                time.sleep(.05)
            if args.suite:
                return run_suite(build, out, env, args.fault, args.headset)
            command = args.client or [str(build/'tests/tests_macos_openxr_loaderless_probe')]
            if args.benchmark:
                command = ['/usr/bin/caffeinate', '-i']+command
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
            (out/'client.txt').write_text(result.stdout+result.stderr)
            (out/'summary.json').write_text(json.dumps(dict(client_exit=result.returncode,
                target=('macos' if args.headset else 'debug_image'), simulated_hmd=not args.headset, command=command), indent=2)+'\n')
            return result.returncode
        finally:
            subprocess.run(['launchctl', 'bootout', domain, str(plist)], timeout=10, check=False)
            if args.suite and (out/'summary.json').exists():
                verify_suite(out, args.fault, args.headset)


def events(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.startswith('{')]


def verify_suite(out, fault_mode, headset=False):
    failed = events(out/'failed.txt')
    start = next(row['time_ns'] for row in failed if row['event'] == 'fault')
    if fault_mode == 'missing':
        duration = next(row['recovery_ns'] for row in failed if row['event'] == 'result')
        finish = start+duration
    else:
        finish = next(row['end_ns'] for row in events(out/'replacement.txt') if row['event'] == 'frame')
        duration = finish-start
    rows = list(csv.DictReader(next(out.glob('*client_frame_map.csv')).open()))
    healthy = [row for row in rows if row['client_slot'] == '0' and row['reused'] == '0']
    during = [row for row in healthy if start <= int(row['latch_ns']) <= finish]
    times = [int(row['latch_ns']) for row in during]
    log = (out/'service.log').read_text()
    samples = sum('rgbaL=(137,188,224,255)' in line and 'rgbaR=(137,188,224,255)' in line for line in log.splitlines())
    quarantines = re.findall(r'GPU reuse quarantine: swapchain=(0x[0-9a-f]+)', log)
    if len(during) < (10 if fault_mode == 'missing' else 1) or samples < 2 or not quarantines:
        raise RuntimeError('GPU rendering/isolation evidence insufficient')
    bad_value = next(row['value'] for row in failed if row['event'] == 'suppressed_signal')
    if re.search(r'producer ready wait: swapchain='+quarantines[0]+r' image=\d+ timeline='+str(bad_value)+r' ', log):
        raise RuntimeError('Missing producer value reached a GPU submission')
    replacement = [row for row in events(out/'replacement.txt') if row['event'] == 'frame']
    replacement_latches = [row for row in rows if row['client_slot'] == '1' and row['reused'] == '0'
                           and replacement[0]['begin_ns'] <= int(row['latch_ns']) <= replacement[-1]['end_ns']]
    if len(replacement_latches) < 10:
        raise RuntimeError('Replacement did not reach actual GPU composition')
    if f'GPU reuse reclaim: swapchain={quarantines[0]}' in log:
        raise RuntimeError('Unsignalled failed-client storage was reclaimed')
    if log.count('starting up...') != 1 or 'producer ready wait' not in log:
        raise RuntimeError('Service restart or missing imported Vulkan dependency evidence')
    summary = json.loads((out/'summary.json').read_text())
    before = [int(row['latch_ns']) for row in healthy if int(row['latch_ns']) < start]
    summary.update(replacement_fresh_latches=len(replacement_latches), fault_mode=fault_mode, healthy_latch_max_gap_before_ms=max((b-a)/1e6 for a,b in zip(before,before[1:])), gpu_readback_matches=samples, healthy_new_frames_during_fault=len(during),
        healthy_latch_max_gap_ms=max((b-a)/1e6 for a,b in zip(times,times[1:])),
        recovery_ms=duration/1e6, recovery_endpoint=('session_loss' if fault_mode == 'missing' else 'replacement_first_frame'), failed_swapchain_retained=True,
        service_start_count=1, status='PASS', note='Offscreen GPU output, not physical display presentation')
    if headset:
        presented = list(csv.DictReader(next(out.glob('*presented.csv')).open()))
        fresh_ids = {row['system_frame_id'] for row in during}
        physical = [row for row in presented if row['frame_id'] in fresh_ids
                    and start <= int(row['presented_monotonic_ns']) <= finish]
        replacement_ids = {row['system_frame_id'] for row in replacement_latches}
        physical_replacement = [row for row in presented if row['frame_id'] in replacement_ids
                                and int(row['presented_monotonic_ns']) > 0]
        completed = list(csv.DictReader(next(out.glob('*present_complete.csv')).open()))
        if not completed or any(row['status'] != '4' or row['shared_event_wait'] != '1' for row in completed):
            raise RuntimeError('Presentation GPU failure or missing shared-event dependency')
        if len(physical) < (10 if fault_mode == 'missing' else 1) or len(physical_replacement) < 10:
            raise RuntimeError('Insufficient actual headset presentation during fault/replacement')
        pts = sorted(int(row['presented_monotonic_ns']) for row in physical)
        summary.update(presentation_gpu_completions=len(completed), presentation_gpu_errors=0,
                       presentation_shared_event_waits=len(completed), healthy_physical_frames_during_fault=len(physical),
                       replacement_physical_frames=len(physical_replacement),
                       healthy_physical_max_gap_ms=max(((b-a)/1e6 for a,b in zip(pts,pts[1:])), default=0),
                       note='Physical CAMetalDrawable presentation callbacks; visual comfort not assessed')
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')


def run_suite(build, out, env, fault_mode, headset=False):
    executable = str(build/'tests/tests_macos_service_gpu_sync')
    clients = []
    results = []
    with (out/'healthy.txt').open('w') as healthy_log:
        healthy = subprocess.Popen([executable, 'normal', '240'], env=env, stdout=healthy_log, stderr=healthy_log)
        clients.append(healthy)
        try:
            deadline = time.monotonic()+10
            while (out/'healthy.txt').read_text().count('"event":"frame"') < 10:
                if healthy.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Healthy client failed before injection')
                time.sleep(.01)
            for name, mode, frames in [('failed', 'missing', '120'), ('replacement', 'normal', '60')]:
                with (out/f'{name}.txt').open('w') as log:
                    client = subprocess.Popen([executable, mode, frames], env=env, stdout=log, stderr=log)
                    clients.append(client)
                    if name == 'failed' and fault_mode == 'disconnect':
                        deadline = time.monotonic()+10
                        while '"event":"fault"' not in (out/'failed.txt').read_text():
                            if client.poll() is not None or time.monotonic() >= deadline:
                                raise RuntimeError('Fault did not arm before disconnection')
                            time.sleep(.005)
                        client.kill()
                    status = client.wait(timeout=15)
                    results.append(dict(client=name, pid=client.pid, exit=status))
                    if status != (-9 if name == 'failed' and fault_mode == 'disconnect' else 0):
                        raise RuntimeError(f'{name} client failed')
            results.append(dict(client='healthy', pid=healthy.pid, exit=healthy.wait(timeout=15)))
            if results[-1]['exit'] != 0:
                raise RuntimeError('Healthy client failed during recovery')
        finally:
            for client in clients:
                if client.poll() is None:
                    client.kill()
                client.wait(timeout=5)
    (out/'summary.json').write_text(json.dumps(dict(target=('macos' if headset else 'debug_image'), clients=results,
        status='PASS', note='GPU trace verification follows service shutdown'), indent=2)+'\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
