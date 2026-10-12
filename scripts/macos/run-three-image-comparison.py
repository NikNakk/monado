#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Compare production service Metal array swapchains; headset runs are user-operated."""
import argparse
import csv
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import threading

from importlib.machinery import SourceFileLoader


PROOF_SAMPLE_INTERVAL = 30
HANG = 'kIOGPUCommandBufferCallbackErrorHang'
# The M5 cross-stack stress workload, looped as competing GPU load.
BACKGROUND_LOAD = {'args': ['concurrent-producers', '3000'],
                   'env': {'GPU_SYNC_TEXTURE_SIZE': '1024', 'GPU_SYNC_COPY_REPEATS': '64',
                           'GPU_SYNC_PRODUCER_UPLOAD': '1'}}
WARMUP_FRAMES = 30
EXPECTED = '(137,188,224,255)'
SAMPLE = re.compile(r'^vk-target frame=(\d+) layer=0 rgbaL=(\([^)]*\)) rgbaC=\([^)]*\) rgbaR=(\([^)]*\))')


def pixel_check(path, log):
    """Check composed pixels on every sampled frame that carried a client layer.

    Startup frames are excluded: the first composed frames can be blank even
    when they latch a client frame. A single blank later frame is recorded,
    not fatal; more than one fails the run.
    """
    mapping = {int(row['system_frame_id']): row
               for row in csv.DictReader(next(path.glob('*client_frame_map.csv')).open())}
    first = min(mapping, default=None)
    samples = []
    for line in log.splitlines():
        match = SAMPLE.match(line)
        if match and first is not None and int(match[1]) >= first+WARMUP_FRAMES and int(match[1]) in mapping:
            frame, left, right = int(match[1]), match[2], match[3]
            row = mapping[frame]
            samples.append({'frame': frame, 'client_frame_id': int(row['client_frame_id']),
                            'reused': row['reused'] == '1', 'left': left, 'right': right,
                            'matched': left == EXPECTED and right == EXPECTED})
    blanks = [sample for sample in samples if not sample['matched']]
    result = {'status': 'PASS' if len(samples) >= 4 and len(blanks) <= 1 else 'FAIL',
              'eligible': len(samples), 'matched': len(samples)-len(blanks),
              'blank_frames': [sample['frame'] for sample in blanks],
              'first_client_frame': first, 'warmup_frames': WARMUP_FRAMES,
              'sample_interval': PROOF_SAMPLE_INTERVAL, 'expected': EXPECTED, 'samples': samples}
    (path/'pixel-check.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=Path('build/arm64'))
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--headset', action='store_true')
    parser.add_argument('--service-env', action='append', default=[], metavar='XRT_NAME=VALUE',
                        help='Passed to every step, e.g. XRT_MACOS_FORCE_CPU_RENDER_WAIT=1')
    parser.add_argument('--background-load', action='store_true',
                        help='Run the GPU sync stress fixture continuously alongside every step')
    args = parser.parse_args()
    if not 120 <= args.frames <= 12000:
        parser.error('--frames must be between 120 and 12000')
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                'source_diff_sha256': hashlib.sha256(subprocess.check_output(['git', 'diff', 'HEAD'])).hexdigest(),
                'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'headset': args.headset, 'frames_per_timing_run': args.frames,
                'service_env': args.service_env,
                'background_load': BACKGROUND_LOAD if args.background_load else None,
                'startup_hang_retries': [], 'binaries': {}}
    for binary in ['tests/tests_macos_service_gpu_sync', 'src/xrt/targets/service/monado-service',
                   'src/xrt/targets/openxr/libopenxr_monado.dylib']:
        manifest['binaries'][binary] = hashlib.sha256((args.build/binary).read_bytes()).hexdigest()
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    scripts = Path(__file__).resolve().parent
    timings = SourceFileLoader('service_timings', str(scripts/'summarize-gpu-sync-service.py')).load_module()
    cases = [('proof-four', 4, False), ('proof-three', 3, False),
             ('A1-four', 4, True), ('B1-three', 3, True),
             ('B2-three', 3, True), ('A2-four', 4, True)]
    if args.headset:
        # The service blocks display sleep only while a client is connected;
        # keep the PS VR2 awake between steps, or nothing is presented.
        subprocess.run(['/usr/bin/caffeinate', '-u', '-t', '2'], check=False)
        subprocess.Popen(['/usr/bin/caffeinate', '-d', '-i', '-w', str(os.getpid())])
    load = start_background_load(args.build, args.output) if args.background_load else None
    try:
        run_cases(args, cases, manifest, scripts, timings)
    finally:
        if load is not None:
            load()
    report = [json.loads((args.output/name/'comparison.json').read_text())
              for name, _, benchmark in cases if benchmark]
    (args.output/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    return 0


def start_background_load(build, output):
    """Loop the stress fixture until the returned function is called."""
    stop = threading.Event()
    env = {**os.environ, **BACKGROUND_LOAD['env']}
    def loop():
        with (output/'background-load.log').open('w') as log:
            while not stop.is_set():
                process = subprocess.Popen([str(build/'tests/tests_macos_gpu_sync')]+BACKGROUND_LOAD['args'],
                                           env=env, stdout=log, stderr=log)
                while process.poll() is None:
                    if stop.wait(0.5):
                        process.terminate()
                        process.wait(timeout=10)
                        return
    thread = threading.Thread(target=loop, daemon=True)
    thread.start()
    def finish():
        stop.set()
        thread.join(timeout=15)
    return finish


def run_step(command, path, manifest, args):
    """Run one step, retrying once after the known startup GPU hang."""
    if subprocess.run(command).returncode == 0:
        return
    log = (path/'service.log').read_text(errors='replace') if (path/'service.log').exists() else ''
    if HANG not in log:
        raise subprocess.CalledProcessError(1, command)
    failed = args.output/'failed-attempts'/(path.name+'-1')
    failed.parent.mkdir(exist_ok=True)
    path.rename(failed)
    manifest['startup_hang_retries'].append(path.name)
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps({'run': path.name, 'startup_gpu_hang': 'retrying once', 'kept': str(failed)}), flush=True)
    subprocess.run(command, check=True)


def run_cases(args, cases, manifest, scripts, timings):
    for name, count, benchmark in cases:
        path = args.output/name
        command = [sys.executable, str(scripts/'run-gpu-sync-service.py'),
                   '--build', str(args.build), '--output', str(path), '--client-timeout', '300']
        if count == 3:
            command += ['--three-images']
        if benchmark:
            command += ['--benchmark']
        if args.headset:
            command += ['--headset']
        if not benchmark:
            command += ['--sample-interval', str(PROOF_SAMPLE_INTERVAL)]
        for item in args.service_env:
            command += ['--service-env', item]
        command += ['--client', str(args.build/'tests/tests_macos_service_gpu_sync'),
                    'array-three' if count == 3 else 'array-four',
                    str(args.frames if benchmark else 240), '290']
        run_step(command, path, manifest, args)
        log = (path/'service.log').read_text()
        events = [json.loads(line) for line in (path/'client.txt').read_text().splitlines()
                  if line.startswith('{')]
        if not any(e.get('status') == 'PASS' for e in events):
            raise RuntimeError(name+': no passing client result')
        # Check actual allocations and service scheduler, rather than env alone.
        if next(e for e in events if e['event'] == 'swapchain')['images'] != count:
            raise RuntimeError(name+': image count mismatch')
        if args.headset:
            with next(path.glob('*_presented.csv')).open() as trace:
                shown = sum(1 for row in csv.DictReader(trace) if int(row['presented_monotonic_ns'] or 0) > 0)
            if shown == 0:
                raise RuntimeError(name+': no frame reached the display (asleep?)')
        if not benchmark:
            if ('Three-image compatibility:' in log) != (count == 3):
                raise RuntimeError(name+': unexpected scheduler')
            check = pixel_check(path, log)
            print(json.dumps({'run': name, 'pixel_check': {k: check[k] for k in
                              ('status', 'eligible', 'matched', 'blank_frames')}}), flush=True)
            if check['status'] != 'PASS':
                raise RuntimeError(f"{name}: composed pixels matched {check['matched']} of "
                                   f"{check['eligible']} samples; see pixel-check.json")
        if benchmark:
            summary = timings.summarize(args.output)
            result = next(row for row in summary if row['run'] == name)
            frames = [e for e in events if e['event'] == 'frame' and e['frame'] > 30]
            result['acquire_wait_us'] = timings.distribution([e['acquire_wait_ns']/1000 for e in frames])
            mapping = list(csv.DictReader(next(path.glob('*client_frame_map.csv')).open()))
            fresh = [int(row['latch_ns']) for row in mapping
                     if row['reused'] == '0' and int(row['client_frame_id']) > 30]
            result['fresh_latches'] = len(fresh)
            result['fresh_latch_gap_us'] = timings.distribution([(b-a)/1000 for a, b in zip(fresh, fresh[1:])])
            result['fresh_latch_hz'] = (len(fresh)-1)*1e9/(fresh[-1]-fresh[0]) if len(fresh) > 1 else 0
            thermal = [json.loads(json.loads(row)['output'])['thermal']
                       for row in (path/'thermal.jsonl').read_text().splitlines()]
            result['thermal_states'] = sorted(set(thermal))
            (path/'comparison.json').write_text(json.dumps(result, indent=2)+'\n')
            print(json.dumps(result), flush=True)


if __name__ == '__main__':
    raise SystemExit(main())
