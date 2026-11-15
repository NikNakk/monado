#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Compare no Metal rate map against a cached all-1.0 map on the simulated HMD."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]


def main():
    # The service runner accepts a positional client command. Keep its options
    # here so --frames/--unity-rate-map are passed to the test application.
    if len(sys.argv) > 1 and sys.argv[1] == 'client':
        _, _, build, mode, frames, *pass_args = sys.argv
        os.environ['PSVR2_TEST_GPU_PASSES'] = pass_args[0] if pass_args else '1'
        os.environ['XR_RUNTIME_JSON'] = str(Path(build)/'openxr_monado-dev.json')
        command = [str(Path(build)/'src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test'),
                   '--frames', frames]
        if mode == 'unity':
            command += ['--unity-rate-map']
        os.execv(command[0], command)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/arm64')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=2400)
    parser.add_argument('--passes', type=int, default=1)
    args = parser.parse_args()
    if not 960 <= args.frames <= 18000 or not 1 <= args.passes <= 64:
        parser.error('frames must be 960–18000 and passes 1–64')
    args.build = args.build.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = dict(head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                    frames=args.frames, passes=args.passes, warmup='discard first 240-sample GPU batch',
                    target='simulated HMD, offscreen debug_image, service compositor', binaries={})
    for binary in ['src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test',
                   'src/xrt/targets/service/monado-service',
                   'src/xrt/targets/openxr/libopenxr_monado.dylib']:
        manifest['binaries'][binary] = hashlib.sha256((args.build/binary).read_bytes()).hexdigest()
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    (args.output/'source.diff').write_text(subprocess.check_output(['git', 'diff'], cwd=ROOT, text=True))
    (args.output/'runner.py').write_text(Path(__file__).read_text())
    results = []
    for name, mode in [('A1', 'normal'), ('B1', 'unity'), ('B2', 'unity'), ('A2', 'normal')]:
        output = args.output/name
        command = [sys.executable, str(ROOT/'scripts/macos/run-gpu-sync-service.py'),
                   '--build', str(args.build), '--output', str(output), '--benchmark',
                   '--client-timeout', '300', '--client', sys.executable, str(Path(__file__).resolve()),
                   'client', str(args.build), mode, str(args.frames), str(args.passes)]
        subprocess.run(command, check=True)
        log = (output/'client.txt').read_text()
        if f'stress GPU passes per eye={args.passes}\n' not in log:
            raise RuntimeError(name+': render pass count mismatch')
        if 'GPU command error' in log or (mode == 'unity' and log.count('identity verified; no resolve') != 2):
            raise RuntimeError(name+': GPU/map verification failed')
        pattern = r'GPU '+('unity-rate-map' if mode == 'unity' else 'normal')+r' n=(\d+) mean=([\d.]+)ms p50=([\d.]+)ms p90=([\d.]+)ms p99=([\d.]+)ms'
        batches = [dict(zip(['n', 'mean_ms', 'p50_ms', 'p90_ms', 'p99_ms'], map(float, match)))
                   for match in re.findall(pattern, log)]
        batches = batches[1:]
        if len(batches) < 2:
            raise RuntimeError(name+': insufficient timing batches')
        thermal = [json.loads(json.loads(line)['output'])['thermal']
                   for line in (output/'thermal.jsonl').read_text().splitlines()]
        result = dict(run=name, mode=mode, samples=int(sum(b['n'] for b in batches)),
                      mean_ms=statistics.mean(b['mean_ms'] for b in batches),
                      median_batch_p50_ms=statistics.median(b['p50_ms'] for b in batches),
                      thermal_states=sorted(set(thermal)), batches=batches)
        results.append(result)
        print(json.dumps(result), flush=True)
        (args.output/'comparison.json').write_text(json.dumps(results, indent=2)+'\n')
    normal = statistics.mean(r['mean_ms'] for r in results if r['mode'] == 'normal')
    unity = statistics.mean(r['mean_ms'] for r in results if r['mode'] == 'unity')
    delta = dict(normal_mean_ms=normal, unity_mean_ms=unity, delta_ms=unity-normal,
                 delta_percent=100*(unity/normal-1))
    (args.output/'delta.json').write_text(json.dumps(delta, indent=2)+'\n')
    print(json.dumps(delta), flush=True)


if __name__ == '__main__':
    main()
