#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Client-only adapter for scripts/macos/run-gpu-sync-service.py --client."""
import argparse
import collections
import csv
import json
import os
from pathlib import Path
import subprocess
import time
import sys
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--service-output', type=Path, help='Launch an isolated simulated service around this client')
p.add_argument('--headset', action='store_true', help='User-operated physical run; requires --service-output')
p.add_argument('--build', type=Path, required=True)
p.add_argument('--driver', type=Path, required=True)
p.add_argument('--icd', type=Path, required=True)
p.add_argument('--molten-icd', type=Path, required=True)
p.add_argument('--mode', choices=['probe', 'hello', 'faults', 'budget'], default='probe')
p.add_argument('--hello', type=Path)
p.add_argument('--seconds', type=int, default=8, help='Hello viewing interval, 1 to 240 seconds')
p.add_argument('--bad-library', type=Path)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--bgra', action='store_true')
p.add_argument('--hosted', action='store_true')
p.add_argument('--three-images', action='store_true')
p.add_argument('--backend', choices=['kosmickrisp', 'moltenvk'], default='kosmickrisp')
a = p.parse_args()
if not 1 <= a.seconds <= 240:
    p.error('--seconds must be between 1 and 240')
if a.service_output:
    args = sys.argv[1:]
    i = args.index('--service-output')
    del args[i:i + 2]
    if '--headset' in args:
        args.remove('--headset')
    with tempfile.TemporaryDirectory(prefix='kk-openxr-launch-') as directory:
        launcher = Path(directory) / 'client.py'
        launcher.write_text('import runpy,sys\nsys.argv=' + repr([str(Path(__file__).resolve())] + args) +
                            '\nrunpy.run_path(sys.argv[0],run_name="__main__")\n')
        service = Path(__file__).resolve().parents[2] / 'scripts/macos/run-gpu-sync-service.py'
        command = [sys.executable, str(service), '--build', str(a.build),
                   '--output', str(a.service_output), '--client-timeout', str(max(60, a.seconds + 20))]
        if a.headset:
            command.append('--headset')
        if a.three_images:
            command.append('--three-images')
        command += ['--client', sys.executable, str(launcher)]
        status = subprocess.run(command).returncode
        if status:
            sys.exit(status)
        scopes = collections.defaultdict(collections.Counter)
        for trace in a.service_output.glob('*client_gpu.csv'):
            for row in csv.DictReader(trace.open()):
                scopes[row['scope_id']][row['event']] += 1
        if a.mode == 'faults':
            failed = [events for events in scopes.values() if events['semaphore_failed']]
            if len(failed) != 1 or failed[0]['semaphore_failed'] != 4 or failed[0]['semaphore_ready'] or failed[0]['scheduled']:
                raise RuntimeError('missing-signal readiness invariant failed')
            scheduled = [events['scheduled'] for events in scopes.values()]
            if 600 not in scheduled or 180 not in scheduled:
                raise RuntimeError('healthy/replacement scheduling evidence missing')
        elif a.mode == 'budget':
            if sum(events['semaphore_failed'] for events in scopes.values()) != 32:
                raise RuntimeError('repeated failure count did not reach the frontend cap')
            if sum(events['scheduled'] for events in scopes.values()) != 72:
                raise RuntimeError('healthy repeated-session scheduling evidence missing')
        elif a.mode == 'probe':
            if a.backend == 'moltenvk':
                visible = []
                for trace in a.service_output.glob('*reprojection_source.csv'):
                    visible += [row for row in csv.DictReader(trace.open()) if row['source_valid'] == '1']
                if not visible or max(int(row['client_frame_id']) for row in visible) < 170:
                    raise RuntimeError('baseline compositor consumption evidence missing')
            elif not any(events['scheduled'] == 180 and events['semaphore_ready'] == 180 for events in scopes.values()):
                raise RuntimeError('probe readiness/scheduling evidence missing')
        elif not any(events['scheduled'] > 0 for events in scopes.values()):
            raise RuntimeError('hello scheduling evidence missing')
        sys.exit(0)
if a.headset:
    p.error('--headset requires --service-output')
a.output.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, XR_RUNTIME_JSON=str(a.build / 'openxr_monado-dev.json'),
           XRT_MACOS_KOSMICKRISP='1', XRT_MACOS_KOSMICKRISP_LIBRARY=str(a.driver),
           VK_DRIVER_FILES=str(a.icd) + ':' + str(a.molten_icd),
           MACOS_OPENXR_VULKAN_PROBE_API_1_2='1',
           MACOS_OPENXR_VULKAN_PROBE_FRAMES='180',
           MACOS_OPENXR_VULKAN_PROBE_PER_VIEW_SWAPCHAINS='1',
           MACOS_OPENXR_VULKAN_PROBE_COLOR_ATTACHMENT_CLEAR='1')
if a.backend == 'moltenvk':
    env.pop('XRT_MACOS_KOSMICKRISP', None)
    env.pop('XRT_MACOS_KOSMICKRISP_LIBRARY', None)
    env['VK_DRIVER_FILES'] = str(a.molten_icd)
if a.bgra:
    env['MACOS_OPENXR_VULKAN_PROBE_BGRA'] = '1'
if a.hosted:
    env['XRT_MACOS_CLIENT_COMPOSITOR'] = '1'
exe = str(a.build / 'tests/tests_macos_openxr_vulkan_probe')
processes = []
results = {}
def spawn(name, extra=None, command=None):
    f = (a.output / (name + '.log')).open('w')
    child = subprocess.Popen(command or [exe], env=dict(env, **(extra or {})),
                             stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    processes.append((child, f))
    return child

def wait(name, child, expected, timeout=15):
    start = time.monotonic()
    try:
        child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        results[name] = {'timeout': True}
        raise
    results[name] = {'exit': child.returncode, 'wait_seconds': time.monotonic() - start}
    if (expected == 'success' and child.returncode != 0) or (expected == 'reject' and child.returncode != 1):
        raise RuntimeError(name + ': unexpected exit ' + str(child.returncode))

try:
    if a.mode == 'hello':
        if not a.hello:
            p.error('--hello is required for hello mode')
        child = spawn('hello', command=[str(a.hello), '-g', 'Vulkan2', '-bm', 'Opaque', '-v'])
        deadline = time.monotonic() + a.seconds
        while time.monotonic() < deadline and not (a.output / 'STOP').exists():
            if child.poll() is not None:
                break
            time.sleep(.1)
        if child.poll() is not None:
            results['hello'] = {'exit': child.returncode, 'early_exit': True}
            raise RuntimeError('hello exited before the rendering interval finished')
        child.stdin.write(b'\n')
        child.stdin.flush()
        wait('hello', child, 'success', 8)
    elif a.mode == 'budget':
        wait('budget-failed', spawn('budget-failed', {'MACOS_OPENXR_VULKAN_PROBE_REPEAT': '9',
             'MACOS_OPENXR_VULKAN_PROBE_FRAMES': '6',
             'MACOS_OPENXR_VULKAN_PROBE_SUPPRESS_RUNTIME_SIGNAL': '1'}), 'reject', 45)
        failed_log = (a.output / 'budget-failed.log').read_text()
        if failed_log.count('process frontend budget exhausted') != 1 or 'iteration 9/9 exit=1' not in failed_log:
            raise RuntimeError('frontend budget rejection evidence missing')
        wait('budget-healthy', spawn('budget-healthy', {'MACOS_OPENXR_VULKAN_PROBE_REPEAT': '12',
             'MACOS_OPENXR_VULKAN_PROBE_FRAMES': '6'}), 'success', 20)
        if 'process frontend budget exhausted' in (a.output / 'budget-healthy.log').read_text():
            raise RuntimeError('healthy frontend reservations were not released')
    elif a.mode == 'probe':
        wait('probe', spawn('probe'), 'success')
    else:
        healthy = spawn('healthy', {'MACOS_OPENXR_VULKAN_PROBE_FRAMES': '600'})
        time.sleep(.5)
        cases = [('missing', {'MACOS_OPENXR_VULKAN_PROBE_SUPPRESS_RUNTIME_SIGNAL': '1',
                              'MACOS_OPENXR_VULKAN_PROBE_OVERLAY': '1'}),
                 ('bad-library', {'XRT_MACOS_KOSMICKRISP_LIBRARY': str(a.bad_library or a.output / 'absent.dylib')}),
                 ('unsupported-array', {'MACOS_OPENXR_VULKAN_PROBE_PER_VIEW_SWAPCHAINS': '0'}),
                 ('wrong-driver', {'VK_DRIVER_FILES': str(a.molten_icd)})]
        for name, extra in cases:
            wait(name, spawn(name, extra), 'reject', 12)
        if 'XR_ERROR_SESSION_LOST' not in (a.output / 'missing.log').read_text():
            raise RuntimeError('missing producer did not observe session loss')
        victim = spawn('killed', {'MACOS_OPENXR_VULKAN_PROBE_OVERLAY': '1',
                                  'MACOS_OPENXR_VULKAN_PROBE_FRAMES': '600'})
        time.sleep(.6)
        if victim.poll() is not None:
            raise RuntimeError('victim exited before kill')
        victim.kill()
        victim.wait(timeout=5)
        results['killed'] = {'exit': victim.returncode}
        wait('replacement', spawn('replacement', {'MACOS_OPENXR_VULKAN_PROBE_OVERLAY': '1'}), 'success')
        wait('healthy', healthy, 'success')
        for name, count in [('replacement', 180), ('healthy', 600)]:
            if 'submitted ' + str(count) + ' projection frames' not in (a.output / (name + '.log')).read_text():
                raise RuntimeError(name + ': missing frame submission evidence')
finally:
    for child, f in processes:
        if child.poll() is None:
            child.kill()
            child.wait()
        f.close()
    (a.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
