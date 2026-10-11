#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Finite headless GPU correctness/latency runs; retain raw JSONL and telemetry."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import time

SCENARIOS = ['normal', 'delayed', 'missing', 'future', 'nonmonotonic', 'exit-before',
             'exit-after', 'consumer-delay', 'multi-queue', 'broken-queue', 'reuse', 'idle', 'cancel-callback', 'exit-inflight', 'consumer-gpu-delay', 'concurrent-producers']

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe', default='build/arm64/tests/tests_macos_gpu_sync')
    p.add_argument('--output', required=True)
    p.add_argument('--frames', type=int, default=120)
    p.add_argument('--repeats', type=int, default=1)
    p.add_argument('--scenario', choices=SCENARIOS, action='append')
    p.add_argument('--concurrent', type=int, default=1)
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--keep-awake', action='store_true', help='Temporarily inhibit idle system sleep during each run')
    a = p.parse_args()
    if not 1 <= a.concurrent <= 8 or not 1 <= a.repeats <= 1000:
        p.error('concurrency/repeat bounds exceeded')
    out = Path(a.output); out.mkdir(parents=True, exist_ok=True)
    def capture(command):
        try:
            r = subprocess.run(command, text=True, capture_output=True, timeout=5)
            return dict(command=command, exit=r.returncode, stdout=r.stdout, stderr=r.stderr)
        except (OSError, subprocess.TimeoutExpired) as e:
            return dict(command=command, unavailable=str(e))
    telemetry = dict(start_time=time.time(), game_mode='not requested; headless CLI', keep_awake=a.keep_awake,
                     commit=capture(['git', 'rev-parse', 'HEAD']),
                     branch=capture(['git', 'branch', '--show-current']),
                     dirty=capture(['git', 'status', '--short']),
                     os=capture(['/usr/bin/sw_vers']),
                     cpu=capture(['/usr/sbin/sysctl', 'machdep.cpu.brand_string', 'hw.model']),
                     power=capture(['/usr/bin/pmset', '-g', 'batt']),
                     settings=capture(['/usr/bin/pmset', '-g', 'custom']),
                     processes=capture(['/bin/ps', '-axo', 'pid,pcpu,pmem,comm']),
                     powermetrics='not collected (requires privileged, platform-specific samplers)',
                     cpu_gpu_frequency='unavailable',
                     config={k: v for k, v in os.environ.items() if k.startswith('GPU_SYNC_')})
    (out/'telemetry.json').write_text(json.dumps(telemetry, indent=2)+'\n')
    jobs = [(s, i) for i in range(a.repeats) for s in (a.scenario or SCENARIOS)]
    def run(job):
        s, i = job
        try:
            command = [a.probe, s, str(a.frames)]
            if a.keep_awake:
                command = ['/usr/bin/caffeinate', '-i'] + command
            r = subprocess.run(command, text=True, capture_output=True, timeout=a.timeout)
            (out/f'{s}-{i:03}.jsonl').write_text(r.stdout)
            (out/f'{s}-{i:03}.stderr').write_text(r.stderr)
            records = [json.loads(line) for line in r.stdout.splitlines()]
            summary = next((x for x in records if x.get('event') == 'result'), {})
            return dict(scenario=s, repeat=i, exit=r.returncode, result=summary,
                        status='PASS' if r.returncode == 0 and summary.get('status') == 'PASS' else 'FAIL')
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as e:
            return dict(scenario=s, repeat=i, status='FAIL', error=str(e))
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.concurrent) as pool:
        results = list(pool.map(run, jobs))
    (out/'summary.json').write_text(json.dumps(results, indent=2)+'\n')
    print(json.dumps(dict(runs=len(results), passed=sum(r['status']=='PASS' for r in results), output=str(out))))
    return int(any(r['status'] != 'PASS' for r in results))

if __name__ == '__main__':
    raise SystemExit(main())
