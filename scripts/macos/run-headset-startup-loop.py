#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Repeat short service startups and count GPU hangs on the first client frames.

Each startup runs one isolated service and the array swapchain client, as in the
three-image comparison's proof step. Headset runs are user-operated: stop the
installed service first. Results stay outside Git.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def displayed_frames(directory):
    """Frames Core Animation reports as shown; 0 when the display was asleep."""
    paths = list(directory.glob('*_presented.csv'))
    if not paths:
        return 0
    with paths[0].open() as trace:
        return sum(1 for row in csv.DictReader(trace) if int(row['presented_monotonic_ns'] or 0) > 0)


def classify(directory, exit_code, headset):
    log = (directory/'service.log').read_text(errors='replace') if (directory/'service.log').exists() else ''
    if 'kIOGPUCommandBufferCallbackErrorHang' in log:
        return 'gpu_hang'
    if 'VK_ERROR_DEVICE_LOST' in log:
        return 'device_lost'
    if 'Failed to claim USB' in log or 'Failed to set USB' in log:
        return 'usb'
    if 'Requested headset is unavailable' in log or exit_code != 0:
        return 'other_failure'
    # A sleeping display presents nothing and hides presentation-path faults.
    if headset and displayed_frames(directory) == 0:
        return 'not_presented'
    return 'pass'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=Path('build/arm64'))
    parser.add_argument('--count', type=int, default=15, help='Startups to run (1–100)')
    parser.add_argument('--images', choices=['three', 'four'], default='three')
    parser.add_argument('--frames', type=int, default=120, help='Client frames per startup (60–2400)')
    parser.add_argument('--offscreen', action='store_true', help='Use the simulated HMD instead of the headset')
    parser.add_argument('--service-env', action='append', default=[], metavar='XRT_NAME=VALUE',
                        help='Passed to each service, e.g. XRT_MACOS_DRAWABLE_SLOT=0')
    parser.add_argument('--gpu-capture', action='store_true',
                        help='Metal GPU capture of the first client submissions; keeps hung captures and the first passing one')
    parser.add_argument('--stop-on-hang', action='store_true', help='Stop after the first GPU hang')
    args = parser.parse_args()
    if not 1 <= args.count <= 100 or not 60 <= args.frames <= 2400:
        parser.error('--count must be 1–100 and --frames 60–2400')
    repo = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    if out.is_relative_to(repo):
        parser.error('keep results outside Git')
    out.mkdir(parents=True, exist_ok=False)
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip()
    awake = None
    if not args.offscreen:
        # The service blocks display sleep only while a client is connected, so
        # an idle user's display can sleep between startups. Wake it, then hold
        # it awake for the whole loop.
        subprocess.run(['/usr/bin/caffeinate', '-u', '-t', '2'], check=False)
        awake = subprocess.Popen(['/usr/bin/caffeinate', '-d', '-i', '-w', str(os.getpid())])
    results = []
    for index in range(1, args.count+1):
        directory = out/f'start-{index:03d}'
        command = [sys.executable, str(repo/'scripts/macos/run-gpu-sync-service.py'), '--build', str(args.build),
                   '--output', str(directory), '--client-timeout', '60', '--sample-interval', '30']
        if args.images == 'three':
            command += ['--three-images']
        if not args.offscreen:
            command += ['--headset']
        for item in args.service_env:
            command += ['--service-env', item]
        if args.gpu_capture:
            command += ['--service-env', f'XRT_MACOS_GPU_CAPTURE_DIR={directory}']
        command += ['--client', str(args.build/'tests/tests_macos_service_gpu_sync'), f'array-{args.images}',
                    str(args.frames), '60']
        started = time.monotonic()
        env = {**os.environ, 'MTL_CAPTURE_ENABLED': '1'} if args.gpu_capture else None
        with (out/'runner.log').open('a') as log:
            exit_code = subprocess.run(command, cwd=repo, env=env, stdout=log, stderr=log, timeout=180).returncode
        outcome = classify(directory, exit_code, not args.offscreen)
        results.append({'start': index, 'outcome': outcome, 'exit': exit_code,
                        'elapsed_s': round(time.monotonic()-started, 1)})
        if args.gpu_capture:
            captures = list(directory.glob('*.gputrace'))
            if outcome == 'pass' and any(r['outcome'] == 'pass' for r in results[:-1]):
                for capture in captures:
                    shutil.rmtree(capture, ignore_errors=True)
                captures = []
            results[-1]['captures'] = [str(capture) for capture in captures]
        print(f'start {index:3d}: {outcome}', flush=True)
        if args.stop_on_hang and outcome == 'gpu_hang':
            break
        if outcome == 'usb':
            print('USB claim failed; power-cycle the headset before continuing', flush=True)
            break
        if outcome == 'not_presented':
            print('No frame reached the display (asleep?); stopping, as later results would be invalid', flush=True)
            break
    counts = {}
    for result in results:
        counts[result['outcome']] = counts.get(result['outcome'], 0)+1
    summary = {'commit': commit, 'images': args.images, 'frames': args.frames, 'headset': not args.offscreen,
               'service_env': args.service_env, 'counts': counts, 'results': results}
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    if awake is not None:
        awake.terminate()
    print(json.dumps({'counts': counts, 'output': str(out)}), flush=True)
    return 0 if set(counts) == {'pass'} else 1


if __name__ == '__main__':
    raise SystemExit(main())
