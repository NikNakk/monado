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
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build/arm64'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--client', nargs='+')
    parser.add_argument('--client-timeout', type=float, default=30, help='Finite client timeout in seconds (1–300)')
    parser.add_argument('--benchmark', action='store_true', help='Disable readback and per-image diagnostic logging')
    parser.add_argument('--perfetto', action='store_true', help='Capture native Perfetto beside CSV (requires XRT_FEATURE_MACOS_PERFETTO)')
    parser.add_argument('--scale-percentage', type=int, help='Override recommended render scale for offscreen client tests (1–200)')
    parser.add_argument('--headset', action='store_true', help='Use the physical headset and display; requires exclusive USB ownership')
    parser.add_argument('--unique-output', action='store_true')
    parser.add_argument('--fault', choices=['missing', 'disconnect'], default='missing')
    parser.add_argument('--budget-suite', action='store_true', help='Test per-client rejection and repeated failed clients against a small global cap')
    parser.add_argument('--suite', action='store_true', help='Test a healthy client, failed overlay and replacement client in one service')
    args = parser.parse_args()
    if not 1 <= args.client_timeout <= 300:
        parser.error('--client-timeout must be between 1 and 300 seconds')
    if args.scale_percentage is not None and (args.headset or not 1 <= args.scale_percentage <= 200):
        parser.error('--scale-percentage requires an offscreen run and a value between 1 and 200')
    if (args.suite or args.budget_suite) and (args.client or args.benchmark):
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
        if args.perfetto:
            env['XRT_MACOS_PERFETTO_TRACE_DIR'] = str(out)
        if args.scale_percentage is not None:
            env['XRT_COMPOSITOR_SCALE_PERCENTAGE'] = str(args.scale_percentage)
        if args.benchmark:
            env.update(XRT_LOG='warn', XRT_COMPOSITOR_LOG_APPLE_SAMPLES='0',
                       XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE='0')
        if not args.benchmark:
            # Fault suites lose sessions on purpose; keep each loss quick.
            env.update(XRT_MACOS_CLIENT_GPU_HUNG_MS='500')
        if args.budget_suite:
            env.update(XRT_MACOS_GPU_CLIENT_OBJECTS='2', XRT_MACOS_GPU_GLOBAL_OBJECTS='8')
        if args.headset:
            env.update(SIMULATED_ENABLE='0', XRT_COMPOSITOR_FORCE_DEBUG_IMAGE='0',
                       XRT_MACOS_PREVENT_DISPLAY_SLEEP='1', PSVR2_CAMERA_STREAMS='0')
        telemetry = {'start_epoch': time.time(), 'workload': ('64x64 Metal quad; physical PS VR2 display' if args.headset else '64x64 Metal quad; simulated HMD; debug_image target'),
                     'benchmark': args.benchmark, 'powermetrics': 'not collected; power/frequency unavailable',
                     'game_mode': 'not requested'}
        if args.client:
            telemetry['workload'] = 'Custom client; '+('physical PS VR2 display' if args.headset else 'simulated HMD; debug_image target')
        telemetry['headset'] = args.headset
        telemetry['perfetto'] = args.perfetto
        telemetry['render_scale_percentage'] = env.get('XRT_COMPOSITOR_SCALE_PERCENTAGE', '140')
        telemetry['gpu_policy'] = {key: env.get(key, default) for key, default in {
            'XRT_MACOS_FRAME_READINESS_MARGIN_US': '6000', 'XRT_MACOS_CLIENT_GPU_HUNG_MS': '5000',
            'XRT_MACOS_GPU_GLOBAL_MB': 'auto', 'XRT_MACOS_GPU_CLIENT_MB': 'auto',
            'XRT_MACOS_GPU_CLIENT_OBJECTS': '64', 'XRT_MACOS_GPU_GLOBAL_OBJECTS': '256'}.items()}
        for key, command in [('power', ['pmset', '-g', 'batt']), ('settings', ['pmset', '-g', 'custom']), ('thermal', ['pmset', '-g', 'therm']),
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
            if args.headset and 'Debug image target used' in (out/'service.log').read_text():
                raise RuntimeError('Requested headset is unavailable; offscreen fallback cannot validate presentation')
            if args.budget_suite:
                return run_budget_suite(build, out, env)
            if args.suite:
                return run_suite(build, out, env, args.fault, args.headset)
            command = args.client or [str(build/'tests/tests_macos_openxr_loaderless_probe')]
            if args.benchmark:
                command = ['/usr/bin/caffeinate', '-i']+command
            stop_telemetry = threading.Event()
            def sample_thermal():
                # Foundation reports the public system thermal state. Sampling
                # uses no OpenXR session, GPU resource or privileged power probe.
                with (out/'thermal.jsonl').open('w') as samples:
                    while True:
                        probe = subprocess.run([str(build/'tests/tests_macos_service_gpu_sync'), 'telemetry'],
                                               capture_output=True, text=True, timeout=5)
                        samples.write(json.dumps(dict(epoch=time.time(), exit=probe.returncode,
                                                     output=probe.stdout.strip()))+'\n')
                        samples.flush()
                        if stop_telemetry.wait(5):
                            break
            sampler = threading.Thread(target=sample_thermal, daemon=True) if args.benchmark else None
            if sampler:
                sampler.start()
            try:
                result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=args.client_timeout)
            finally:
                stop_telemetry.set()
                if sampler:
                    sampler.join(timeout=6)

            (out/'client.txt').write_text(result.stdout+result.stderr)
            (out/'summary.json').write_text(json.dumps(dict(client_exit=result.returncode,
                target=('macos' if args.headset else 'debug_image'), simulated_hmd=not args.headset, command=command), indent=2)+'\n')
            return result.returncode
        finally:
            subprocess.run(['launchctl', 'bootout', domain, str(plist)], timeout=10, check=False)
            if (out/'summary.json').exists():
                if args.budget_suite:
                    verify_budget_suite(out)
                elif args.suite:
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
        status='PENDING', note='GPU trace verification follows service shutdown'), indent=2)+'\n')
    return 0


def run_budget_suite(build, out, env):
    executable = str(build/'tests/tests_macos_service_gpu_sync')
    def client(name, mode, frames):
        with (out/(name+'.txt')).open('w') as log:
            subprocess.run([executable, mode, str(frames), '40'], env=env,
                           stdout=log, stderr=log, check=True, timeout=45)
    with (out/'healthy.txt').open('w') as log:
        healthy = subprocess.Popen([executable, 'normal', '900', '40'], env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic()+10
            while (out/'healthy.txt').read_text().count('"event":"frame"') < 10:
                if healthy.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Healthy budget client did not start')
                time.sleep(.01)
            client('per-client', 'budget', 1)
            for index in range(7):
                client('failed-'+str(index), 'missing', 120)
            client('global-rejected', 'exhausted', 1)
            if healthy.wait(timeout=40) != 0:
                raise RuntimeError('Healthy rendering failed at global allocation cap')
            client('replacement', 'normal', 60)
        finally:
            if healthy.poll() is None:
                healthy.kill()
            healthy.wait(timeout=5)
    (out/'summary.json').write_text(json.dumps(dict(status='PENDING',
        scenario='budget', global_object_cap=8, client_object_cap=2), indent=2)+'\n')
    return 0


def verify_budget_suite(out):
    log = (out/'service.log').read_text()
    quarantines = dict((identity, address) for address, identity in re.findall(r'GPU reuse quarantine: swapchain=(0x[0-9a-f]+).* id=(\d+)', log))
    reclaimed = set(re.findall(r'GPU reuse reclaim:.* id=(\d+)', log))
    retained = {identity: address for identity, address in quarantines.items() if identity not in reclaimed}
    if len(retained) != 7 or log.count('GPU allocation rejected before import') < 2:
        raise RuntimeError('Missing repeated-failure retention or pre-import cap evidence')
    for address in retained.values():
        if re.search(r'producer ready wait: swapchain='+address+r' image=\d+ timeline=22 ', log):
            raise RuntimeError('Repeated failed producer value reached Vulkan')
    failures = [events(out/('failed-'+str(i)+'.txt')) for i in range(7)]
    start = next(r['time_ns'] for r in failures[0] if r['event'] == 'fault')
    last_start = next(r['time_ns'] for r in failures[-1] if r['event'] == 'fault')
    finish = last_start+next(r['recovery_ns'] for r in failures[-1] if r['event'] == 'result')
    mapping = list(csv.DictReader(next(out.glob('*client_frame_map.csv')).open()))
    fresh = [r for r in mapping if r['client_slot'] == '0' and r['reused'] == '0'
             and start <= int(r['latch_ns']) <= finish]
    replacement = [r for r in events(out/'replacement.txt') if r['event'] == 'frame']
    replacement_latches = [r for r in mapping if r['reused'] == '0'
        and replacement[0]['begin_ns'] <= int(r['latch_ns']) <= replacement[-1]['end_ns']]
    peak = max(map(int, re.findall(r' objects=(\d+)', log)))
    if len(fresh) < 100 or len(replacement_latches) < 10 or peak > 8 or log.count('starting up...') != 1:
        raise RuntimeError('Budget exhaustion damaged healthy rendering or replacement recovery')
    for name in ['per-client', 'global-rejected']:
        result = next(r for r in events(out/(name+'.txt')) if r['event'] == 'result')
        if not result.get('budget_rejected') or result['status'] != 'PASS':
            raise RuntimeError('Allocation cap was not explicitly rejected')
    summary = json.loads((out/'summary.json').read_text())
    summary.update(status='PASS', retained_failed_swapchains=len(retained), peak_tracked_objects=peak,
        healthy_fresh_latches_during_failures=len(fresh), replacement_fresh_latches=len(replacement_latches),
        service_start_count=1, pre_import_rejections=log.count('GPU allocation rejected before import'))
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')


if __name__ == '__main__':
    raise SystemExit(main())
