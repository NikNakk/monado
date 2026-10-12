#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Finite M5 stress and paired measurements; keep all raw evidence outside Git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--wine-artifacts', type=Path, required=True)
    parser.add_argument('--wine-runtime', type=Path, required=True)
    parser.add_argument('--chrome', type=Path, required=True)
    parser.add_argument('--processor', type=Path, required=True)
    parser.add_argument('--stress-frames', type=int, default=12000)
    parser.add_argument('--section', choices=['all', 'chromium'], default='all',
                        help='Run browser validation separately after preserving completed stress/upload evidence')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    if out.is_relative_to(repo) or not 1000 <= args.stress_frames <= 100000:
        parser.error('use output outside Git and 1000–100000 stress frames')
    out.mkdir(parents=True, exist_ok=False)
    probe = repo / 'build/arm64/tests/tests_macos_gpu_sync'
    telemetry = repo / 'build/arm64/tests/tests_macos_service_gpu_sync'
    scripts = repo / 'scripts/macos'
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('GPU_SYNC_', 'M3_CHROMIUM_')):
            del env[key]
    steps = []
    def snapshot(command):
        result = subprocess.run(command, cwd=repo, capture_output=True, text=True, timeout=10)
        return {'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    metadata = {
        'commit': snapshot(['git', 'rev-parse', 'HEAD']),
        'dirty': snapshot(['git', 'status', '--short']),
        'os': snapshot(['sw_vers']), 'power': snapshot(['pmset', '-g', 'batt']),
        'background': snapshot(['ps', '-axo', 'pid,pcpu,comm']),
        'game_mode': 'not requested', 'frequency_power_counters': 'not collected',
        'probe_sha256': hashlib.sha256(probe.read_bytes()).hexdigest(),
        'chrome_framework_sha256': hashlib.sha256(
            (args.chrome.parent.parent / 'Frameworks/Chromium Framework.framework/Versions/Current/Chromium Framework').read_bytes()
        ).hexdigest(),
    }
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')

    def run(name, command, settings=None, timeout=600, nominal=False):
        directory = out / name
        directory.mkdir()
        states = []
        if nominal:
            for _ in range(121):
                state = json.loads(subprocess.check_output([str(telemetry), 'telemetry'], text=True, timeout=5))
                states.append(state)
                if state['thermal'] == 0 and not state['low_power']:
                    break
                time.sleep(5)
            else:
                raise RuntimeError('nominal thermal/power condition unavailable')
        stopped = threading.Event()
        def sample():
            with (directory / 'thermal.jsonl').open('w') as log:
                while not stopped.is_set():
                    try:
                        state = json.loads(subprocess.check_output([str(telemetry), 'telemetry'], text=True, timeout=5))
                        log.write(json.dumps({'monotonic_ns': time.monotonic_ns(), **state}) + '\n')
                    except Exception as error:
                        log.write(json.dumps({'error': str(error)}) + '\n')
                    log.flush()
                    stopped.wait(2)
        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        started = time.monotonic()
        try:
            with (directory / 'stdout.txt').open('w') as stdout, (directory / 'stderr.txt').open('w') as stderr:
                result = subprocess.run(command, cwd=repo, env={**env, **(settings or {})},
                                        stdout=stdout, stderr=stderr, timeout=timeout)
            step = {'name': name, 'command': command, 'settings': settings or {},
                    'exit': result.returncode, 'elapsed_s': time.monotonic() - started,
                    'nominal_required': nominal, 'preflight': states}
        finally:
            stopped.set()
            sampler.join(timeout=6)
        steps.append(step)
        (out / 'steps.json').write_text(json.dumps(steps, indent=2) + '\n')
        print(json.dumps(step), flush=True)
        if result.returncode:
            raise RuntimeError(f'{name} failed; inspect {directory}')
        return directory

    if args.section == 'all':
        stress = {'GPU_SYNC_TEXTURE_SIZE': '1024', 'GPU_SYNC_COPY_REPEATS': '64',
                  'GPU_SYNC_PRODUCER_UPLOAD': '1'}
        native = run('native-stress', [sys.executable, str(scripts / 'run-gpu-sync.py'),
            '--output', str(out / 'native-stress/results'), '--scenario', 'concurrent-producers',
            '--frames', str(args.stress_frames), '--timeout', '600'], stress)
        wine = run('wine-stress', [sys.executable, str(scripts / 'run-wine-gpu-sync.py'),
            '--artifacts', str(args.wine_artifacts.resolve()), '--wine-runtime', str(args.wine_runtime.resolve()),
            '--output', str(out / 'wine-stress/results'), '--scenario', 'concurrent-producers',
            '--frames', str(args.stress_frames), '--timeout', '600'], stress, timeout=750)
        for directory in (native, wine):
            summary = json.loads((directory / 'results/summary.json').read_text())
            assert len(summary) == 1 and summary[0]['status'] == 'PASS'
            assert summary[0]['result']['consumed'] == 2 * args.stress_frames

        # Pair native/Wine at 60 Hz for 20 s; compare these matched uploads only.
        for name, use_wine in [('upload-A1', False), ('upload-B1', True),
                               ('upload-B2', True), ('upload-A2', False)]:
            settings = {'GPU_SYNC_PRODUCER_UPLOAD': '1', 'GPU_SYNC_TEXTURE_SIZE': '128',
                        'GPU_SYNC_INTERVAL_US': '16667'}
            if use_wine:
                settings['GPU_SYNC_WINE_LAUNCHER'] = str(args.wine_artifacts.resolve() / 'launch.zsh')
            directory = run(name, [str(probe), 'normal', '1200'], settings, timeout=160, nominal=True)
            records = [json.loads(line) for line in (directory / 'stdout.txt').read_text().splitlines()]
            assert records[-1]['status'] == 'PASS' and records[-1]['consumed'] == 1200
            assert records[0]['thermal'] == 0 and records[-1]['thermal_end'] == 0

    # Video pair uses identical service scale, decoder and benchmark duration.
    for name, producer, mode in [('angle-stress', 'angle', 'pixels'),
                                 ('dawn-stress', 'dawn', 'pixels'),
                                 ('video-A1', 'angle', 'benchmark'),
                                 ('video-B1', 'dawn', 'benchmark'),
                                 ('video-B2', 'dawn', 'benchmark'),
                                 ('video-A2', 'angle', 'benchmark')]:
        settings = {'M3_CHROMIUM_PATH': str(args.chrome.resolve()),
                    'M3_CHROMIUM_OUTPUT': str(out / name / 'results'),
                    'M3_CHROMIUM_PRODUCER': producer, 'M3_CHROMIUM_MODE': mode,
                    'M3_CHROMIUM_PIXEL_MS': '15000'}
        command = [sys.executable, str(scripts / 'run-gpu-sync-service.py'),
                   '--output', str(out / name / 'results'), '--perfetto',
                   '--scale-percentage', '20', '--client-timeout', '180']
        if mode == 'benchmark':
            command += ['--benchmark']
        command += ['--client', '/opt/homebrew/bin/node', str(scripts / 'run-chromium-gpu-sync.mjs')]
        directory = run(name, command, settings, timeout=210, nominal=mode == 'benchmark')
        run(name + '-validation', [sys.executable, str(scripts / 'check-chromium-gpu-sync.py'),
            str(directory / 'results'), '--processor', str(args.processor.resolve())], timeout=120)
    print(json.dumps({'status': 'PASS', 'steps': len(steps), 'output': str(out)}), flush=True)


if __name__ == '__main__':
    main()
