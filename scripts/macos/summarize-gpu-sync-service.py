#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Summarize comparable offscreen service timing runs (exclude 30 warmup frames)."""
import csv
import json
import math
from pathlib import Path
import sys


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return {name: values[min(len(values)-1, math.ceil(len(values)*fraction)-1)]
            for name, fraction in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]}


def summarize(root):
    results = []
    for path in sorted(root.glob('*')):
        if not path.is_dir() or not (path/'client.txt').exists():
            continue
        frames = [json.loads(line) for line in (path/'client.txt').read_text().splitlines()
                  if line.startswith('{"event":"frame"')]
        measured = [row for row in frames if row['frame'] > 30]
        rows = list(csv.DictReader(next(path.glob('*renderer_stage.csv')).open()))
        ready = list(csv.DictReader(next(path.glob('*client_gpu.csv')).open()))
        mapping = list(csv.DictReader(next(path.glob('*client_frame_map.csv')).open()))
        by_id = {row['frame']: row for row in frames}
        latch_latency = [(int(row['latch_ns'])-by_id[int(row['client_frame_id'])]['begin_ns'])/1000
                         for row in mapping if row['reused'] == '0' and int(row['client_frame_id']) > 30
                         and int(row['client_frame_id']) in by_id]
        result = dict(run=path.name, frames=len(frames),
                      throughput_hz=(len(measured)-1)*1e9/(measured[-1]['end_ns']-measured[0]['end_ns']),
                      end_frame_us=distribution([(row['end_ns']-row['begin_ns'])/1000 for row in measured]),
                      cpu_readiness_wait_us=distribution([int(row['duration_ns'])/1000 for row in ready
                          if row['event']=='semaphore_ready' and int(row['client_frame_id']) > 30]),
                      end_frame_entry_to_latch_us=distribution(latch_latency),
                      reused_latches=sum(row['reused']=='1' for row in mapping))
        for stage in ['queue_submit','draw_dispatch','previous_fence_wait']:
            result[stage+'_us'] = distribution([(int(row['end_ns'])-int(row['begin_ns']))/1000 for row in rows
                                               if row['stage']==stage and int(row['frame_id']) > 30])
        results.append(result)
    (root/'distributions.json').write_text(json.dumps(results, indent=2)+'\n')
    return results


if __name__ == '__main__':
    print(json.dumps(summarize(Path(sys.argv[1])), indent=2))
